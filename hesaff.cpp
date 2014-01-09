/*
 * Copyright (C) 2008-12 Michal Perdoch
 * All rights reserved.
 *
 * This file is part of the HessianAffine detector and is made available under
 * the terms of the BSD license (see the COPYING file).
 *
 */

// Main File. Includes and uses the other files
//

#include <iostream>
#include <fstream>
#include <string>
#include <opencv2/core/core.hpp>

#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include "pyramid.h"
#include "helpers.h"
#include "affine.h"
#include "siftdesc.h"
#include "hesaff.h"

#define make_str(str_name, stream_input) \
            std::string str_name;\
            {std::stringstream tmp_sstm;\
             tmp_sstm << stream_input;\
             str_name = tmp_sstm.str();\
            };

#ifdef MYDEBUG
#undef MYDEBUG
#endif
#define MYDEBUG

#ifdef MYDEBUG
#define print(msg) std::cout << msg << std::endl;
#define write(msg) std::cout << msg;
#else
#define print(msg);
#endif

typedef unsigned char uint8;

struct Keypoint
{
   float x, y, s;
   float a11,a12,a21,a22;
   float response;
   int type;
   uint8 desc[128];
};


void rotate_downwards(float &a11, float &a12, float &a21, float &a22)
{
    //same as rectify_up_is_up but doest remove scale
    double a = a11, b = a12, c = a21, d = a22;
    double absdet_ = abs(a * d - b * c);
    double b2a2 = sqrt(b * b + a * a);
    //double sqtdet_ = sqrt(absdet_);
    //-
    a11 = b2a2;
    a12 = 0;
    a21 = (d * b + c * a) / (b2a2);
    a22 = absdet_ / b2a2;
}


void invE_to_invA(cv::Mat& invE, float &a11, float &a12, float &a21, float &a22)
{
    SVD svd_invE(invE, SVD::FULL_UV);
    float *diagE = (float *)svd_invE.w.data;
    diagE[0] = 1.0f / sqrt(diagE[0]);
    diagE[1] = 1.0f / sqrt(diagE[1]);
    // build new invA
    cv::Mat invA_ = svd_invE.u * cv::Mat::diag(svd_invE.w);
    a11 = invA_.at<float>(0,0);
    a12 = invA_.at<float>(0,1);
    a21 = invA_.at<float>(1,0);
    a22 = invA_.at<float>(1,1);
    // Rectify it (maintain scale)
    rotate_downwards(a11, a12, a21, a22);
}


cv::Mat invA_to_invE(float &a11, float &a12, float &a21, float &a22, float& s, float& desc_factor)
{
    float sc = desc_factor * s;
    cv::Mat invA = (cv::Mat_<float>(2,2) << a11, a12, a21, a22);

    //-----------------------
    // Convert invA to invE format
    SVD svd_invA(invA, SVD::FULL_UV);
    float *diagA = (float *)svd_invA.w.data;
    diagA[0] = 1.0f / (diagA[0] * diagA[0] * sc * sc);
    diagA[1] = 1.0f / (diagA[1] * diagA[1] * sc * sc);
    cv::Mat invE = svd_invA.u * cv::Mat::diag(svd_invA.w) * svd_invA.u.t();
    return invE;
}



struct AffineHessianDetector :
    /*extends*/ public HessianDetector, AffineShape, HessianKeypointCallback, AffineShapeCallback
{
public:
    // Member variables
    const cv::Mat image;
    SIFTDescriptor sift;
    std::vector<Keypoint> keys;

public:
    // Constructor
    AffineHessianDetector(const cv::Mat &image,
                          const PyramidParams &par,
                          const AffineShapeParams &ap,
                          const SIFTDescriptorParams &sp):
        HessianDetector(par), AffineShape(ap), image(image), sift(sp)
    {
        //Inherited from pyramid.h HessianDetector
        this->setHessianKeypointCallback(this);
        // Inherited from affine.h AffineShape
        this->setAffineShapeCallback(this);
    }

    int detect()
    {
        // Reset counters
        this->detectPyramidKeypoints(this->image);
        return this->keys.size();
    }

    void exportArrays(int nKpts, float *kpts, uint8 *desc)
    {
        // Assumes the arrays have been preallocated
        /*  Build output arrays */
        for (size_t fx=0; fx < nKpts; fx++)
        {
            Keypoint &k = keys[fx];
            float x, y, a, b, c, d, s, det;
            float sc = AffineShape::par.mrSize * k.s;
            size_t rowk = fx * 5;
            size_t rowd = fx * 128;
            // given kpts in invA format
            det = k.a11 * k.a22 - k.a12 * k.a21;
            x = k.x;
            y = k.y;
            // Incorporate the scale
            a = sc * k.a11 / (det);
            b = sc * k.a12 / (det);
            c = sc * k.a21 / (det);
            d = sc * k.a22 / (det);

            kpts[rowk + 0] = x;
            kpts[rowk + 1] = y;
            kpts[rowk + 2] = a;
            kpts[rowk + 3] = c;
            kpts[rowk + 4] = d;

            // Assign Descriptor Output
            for (size_t ix = 0; ix < 128; ix++)
            {
                desc[rowd + ix] = uint8(k.desc[ix]);
            }
        }
    }

    void extractDesc(int nKpts, float* kpts, uint8* desc)
        {
        float x, y, ia, ib, ic, id;
        float sc;
        float a11, a12, a21, a22, s;
        for(int fx=0; fx < (nKpts); fx++)
            {
            // 2D Array offsets
            size_t rowk = fx * 5;
            size_t rowd = fx * 128;
            //Read a keypoint from the file
            x = kpts[rowk + 0];
            y = kpts[rowk + 1];
            // We are currently using inv(A) format in HotSpotter
            ia = kpts[rowk + 2];
            ib = 0;
            ic = kpts[rowk + 3];
            id = kpts[rowk + 4];
            // Extract scale.
            sc = sqrt(abs((ia * id) - (ib * ic)));
            // Deintegrate scale. Keep invA format
            s  = (sc / AffineShape::par.mrSize); // scale
            a11 = ia / sc;
            a12 = 0;
            a21 = ic / sc;
            a22 = id / sc;
            #ifdef MYDEBUG
            if (fx == 0)
                {
                //print("[extractDesc.c]    sc = "  << sc);
                //print("[extractDesc.c] iabcd = [" << ia << ", " << ib << ", " << ic << ", " << id << "] ");
                //print("[extractDesc.c]    xy = (" <<  x << ", " <<  y << ") ");
                //print("[extractDesc.c]  abcd = [" << a11 << ", " << a12 << ", " << a21 << ", " << a22 << "] " << "s = " << s);
                }
            #endif
            // now sample the patch (populates this->patch)
            if (!this->normalizeAffine(this->image, x, y, s, a11, a12, a21, a22)) //affine.cpp
                {
                // compute SIFT descriptor
                this->sift.computeSiftDescriptor(this->patch);
                // Populate output descriptor
                for (int ix=0; ix<128; ix++)
                    {
                    desc[(fx * 128) + ix] = (uint8) this->sift.vec[ix];
                    }
                }
            else
                {
                print("Failure!");
                }
            }
        }

    void write_features(char* img_fpath)
    {
        // Write text keypoints to disk
        char suffix[] = ".hesaff.sift";
        int len = strlen(img_fpath)+strlen(suffix)+1;
        #ifdef WIN32
        char* out_fpath = new char[len];
        #else
        char out_fpath[len];
        #endif
        snprintf(out_fpath, len, "%s%s", img_fpath, suffix); out_fpath[len-1]=0;
        std::ofstream out(out_fpath);
        this->exportKeypoints(out);
        // Clean Up
        #ifdef WIN32
        delete[] out_fpath;
        #endif
    }

    void exportKeypoints(std::ostream &out)
    {
        /*Writes text keypoints in the invE format
        * [iE_a, iE_b]
        * [iE_b, iE_d]
        */
        out << 128 << std::endl;
        int nKpts = keys.size();
        print("Writing " << nKpts << " keypoints");
        out << nKpts << std::endl;
        for (size_t i=0; i<nKpts; i++)
        {
            Keypoint &k = keys[i];
            float sc = AffineShape::par.mrSize * k.s;
            // Grav invA keypoints
            cv::Mat invA = (cv::Mat_<float>(2,2) << k.a11, k.a12, k.a21, k.a22);
            // Integrate the scale via signular value decomposition
            // Remember
            // A     = U *  S  * V.T
            // invA  = V * 1/S * U.T
            // E     = X *  W  * X.T
            // invE  = Y *  W  * X.T
            // E     = A.T  * A
            // invE  = invA * invA.T
            // X == Y, because E is symmetric
            // W == S^2
            // X == V

            // Decompose invA
            SVD svd_invA(invA, SVD::FULL_UV);
            float *diag_invA = (float *)svd_invA.w.data;
            // Integrate scale into 1/S and take squared inverst to make 1/W
            diag_invA[0] = 1.0f / (diag_invA[0]*diag_invA[0]*sc*sc);
            diag_invA[1] = 1.0f / (diag_invA[1]*diag_invA[1]*sc*sc);
            // Build the matrix invE
            // (I dont understand why U here, but it preserves the rotation I guess)
            // invE = (V * 1/S * U.T) * (U * 1/S * V.T)
            cv::Mat invE = svd_invA.u * cv::Mat::diag(svd_invA.w) * svd_invA.u.t();
            // Write inv(E) to out stream
            out << k.x << " " << k.y << " "
                << invE.at<float>(0,0) << " "
                << invE.at<float>(0,1) << " "
                << invE.at<float>(1,1);
            for (size_t i=0; i<128; i++)
                out << " " << int(k.desc[i]);
            out << std::endl;
        }
    }


    void onHessianKeypointDetected(const cv::Mat &blur, float x, float y,
            float s, float pixelDistance,
            int type, float response)
        {
        findAffineShape(blur, x, y, s, pixelDistance, type, response);
        }

    void onAffineShapeFound(const cv::Mat &blur, float x, float y,
            float s, float pixelDistance,
            float a11, float a12,
            float a21, float a22,
            int type, float response,
            int iters)
        {
        // detectPyramidKeypoints -> detectOctaveKeypoints -> localizeKeypoint -> findAffineShape -> onAffineShapeFound
        // convert shape into a up is up frame
        rectifyAffineTransformationUpIsUp(a11, a12, a21, a22); //Helper
        // now sample the patch (populates this->patch)
        if (!normalizeAffine(this->image, x, y, s,
                    a11, a12, a21, a22)) //affine.cpp
            {
            // compute SIFT
            sift.computeSiftDescriptor(this->patch);
            // store the keypoint
            keys.push_back(Keypoint());
            Keypoint &k = keys.back();
            k.x = x; k.y = y; k.s = s;
            k.a11 = a11; k.a12 = a12;
            k.a21 = a21; k.a22 = a22;
            k.response = response;
            k.type = type;
            // store the descriptor
            for (int i=0; i<128; i++)
                {
                k.desc[i] = (uint8) sift.vec[i];
                }
            }
        }


};
// END class AffineHessianDetector

#ifdef __cplusplus
extern "C" {
#endif
typedef void*(*allocer_t)(int, int*);

//http://nbviewer.ipython.org/github/pv/SciPy-CookBook/blob/master/ipython/Ctypes.ipynb

extern HESAFF_EXPORT int detect(AffineHessianDetector* detector)
{
    print("detector->detect");
    int nKpts = detector->detect();
    print("nKpts = " << nKpts);
    return nKpts;
}


extern HESAFF_EXPORT AffineHessianDetector* new_hesaff_from_params(char* img_fpath,
        // Pyramid Params
        int   numberOfScales,
        float threshold,
        float edgeEigenValueRatio,
        int   border,
        // Affine Params Shape
        int   maxIterations,
        float convergenceThreshold,
        int   smmWindowSize,
        float mrSize,
        // SIFT params
        int spatialBins,
        int orientationBins,
        float maxBinValue,
        // Shared Pyramid + Affine
        float initialSigma,
        // Shared SIFT + Affine
        int patchSize,
        // My Params
        float min_scale,
        float max_scale)
{
    print("making detector for " << img_fpath);
    print("make hesaff. img_fpath = " << img_fpath);
    // Read in image and convert to uint8
    cv::Mat tmp = cv::imread(img_fpath);
    cv::Mat image(tmp.rows, tmp.cols, CV_32FC1, Scalar(0));
    float *imgout = image.ptr<float>(0);
    uint8 *imgin  = tmp.ptr<uint8>(0);
    for (size_t i=tmp.rows*tmp.cols; i > 0; i--)
    {
        *imgout = (float(imgin[0]) + imgin[1] + imgin[2])/3.0f;
        imgout++;
        imgin+=3;
    }

    // Define params
    SIFTDescriptorParams siftParams;
    PyramidParams pyrParams;
    AffineShapeParams affShapeParams;

    // Copy Pyramid params
    pyrParams.numberOfScales            = numberOfScales;
    pyrParams.threshold                 = threshold;
    pyrParams.edgeEigenValueRatio       = edgeEigenValueRatio;
    pyrParams.border                    = border;
    pyrParams.initialSigma              = initialSigma;

    // Copy Affine Shape params
    affShapeParams.maxIterations        = maxIterations;
    affShapeParams.convergenceThreshold = convergenceThreshold;
    affShapeParams.smmWindowSize        = smmWindowSize;
    affShapeParams.mrSize               = mrSize;
    affShapeParams.initialSigma         = initialSigma;
    affShapeParams.patchSize            = patchSize;

    // Copy SIFT params
    siftParams.spatialBins              = spatialBins;
    siftParams.orientationBins          = orientationBins;
    siftParams.maxBinValue              = maxBinValue;
    siftParams.patchSize                = patchSize;


#ifdef MYDEBUG
    print("pyrParams.numberOfScales      = " << pyrParams.numberOfScales);
    print("pyrParams.threshold           = " << pyrParams.threshold);
    print("pyrParams.edgeEigenValueRatio = " << pyrParams.edgeEigenValueRatio);
    print("pyrParams.border              = " << pyrParams.border);
    print("pyrParams.initialSigma        = " << pyrParams.initialSigma);
    print("affShapeParams.maxIterations        = " << affShapeParams.maxIterations);
    print("affShapeParams.convergenceThreshold = " << affShapeParams.convergenceThreshold);
    print("affShapeParams.smmWindowSize        = " << affShapeParams.smmWindowSize);
    print("affShapeParams.mrSize               = " << affShapeParams.mrSize);
    print("affShapeParams.initialSigma         = " << affShapeParams.initialSigma);
    print("affShapeParams.patchSize            = " << affShapeParams.patchSize);
    print("siftParams.spatialBins     = " << siftParams.spatialBins);
    print("siftParams.orientationBins = " << siftParams.orientationBins);
    print("siftParams.maxBinValue     = " << siftParams.maxBinValue);
    print("siftParams.patchSize       = " << siftParams.patchSize);
    print("myParams.min_scale       = " << min_scale);
    print("myParams.max_scale       = " << max_scale);

#endif

    // Create detector
    AffineHessianDetector* detector = new AffineHessianDetector(image, pyrParams, affShapeParams, siftParams);
    return detector;
}

extern HESAFF_EXPORT AffineHessianDetector* new_hesaff(char* img_fpath)
{
    // Pyramid Params
    int   numberOfScales = 3;
    float threshold = 16.0f / 3.0f;
    float edgeEigenValueRatio = 10.0f;
    int   border = 5;
    // Affine Params Shape
    int   maxIterations = 16;
    float convergenceThreshold = 0.05;
    int   smmWindowSize = 19;
    float mrSize = 3.0f * sqrt(3.0f);
    // SIFT params
    int spatialBins = 4;
    int orientationBins = 8;
    float maxBinValue = 0.2f;
    // Shared Pyramid + Affine
    float initialSigma = 1.6f;
    // Shared SIFT + Affine
    int patchSize = 41;
    // My params
    float min_scale = -1;
    float max_scale = -1;

    AffineHessianDetector* detector = new_hesaff_from_params(img_fpath,
            numberOfScales, threshold, edgeEigenValueRatio, border,
            maxIterations, convergenceThreshold, smmWindowSize, mrSize,
            spatialBins, orientationBins, maxBinValue, initialSigma, patchSize,
            min_scale, max_scale);
    return detector;
}

extern HESAFF_EXPORT void extractDesc(AffineHessianDetector* detector, int nKpts, float* kpts, uint8* desc)
{
    print("detector->extractDesc");
    detector->extractDesc(nKpts, kpts, desc);
    print("extracted nKpts = " << nKpts);
}

extern HESAFF_EXPORT void exportArrays(AffineHessianDetector* detector, int nKpts, float *kpts, uint8 *desc)
{
    print("detector->exportArrays(" << nKpts << ")");
    //print("detector->exportArrays kpts[0]" << kpts[0] << ")");
    //print("detector->exportArrays desc[0]" << (int) desc[0] << ")");
    detector->exportArrays(nKpts, kpts, desc);
    //print("detector->exportArrays kpts[0]" << kpts[0] << ")");
    //print("detector->exportArrays desc[0]" << (int) desc[0] << ")");
    print("FINISHED detector->exportArrays");
}

extern HESAFF_EXPORT void writeFeatures(AffineHessianDetector* detector, char* img_fpath)
{
    print("detector->write_features");
    detector->write_features(img_fpath);
}

#ifdef __cplusplus
}
#endif

int main(int argc, char **argv)
{
    if (argc>1)
        {
        print("[hesaff.c] main()")
        char* img_fpath = argv[1];
        int nKpts;
        AffineHessianDetector* detector = new_hesaff(img_fpath);
        nKpts = detect(detector);
        writeFeatures(detector, img_fpath);
        }
   else
       {
       printf("\nUsage: ell_desc image_name.png kpts_file.txt\nDescribes elliptical keypoints (with gravity vector) given in kpts_file.txt using a SIFT descriptor.\n\n");
       }
}
