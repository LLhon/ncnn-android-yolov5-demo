// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2022 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "layer.h"
#include "net.h"

#if defined(USE_NCNN_SIMPLEOCV)
#include "simpleocv.h"
#else
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#endif
#include <float.h>
#include <stdio.h>
#include <vector>

//android
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/log.h>

//ncnn
#include "net.h"
#include "benchmark.h"

static ncnn::Net yolov5;

static ncnn::UnlockedPoolAllocator g_blob_pool_allocator;
static ncnn::PoolAllocator g_workspace_pool_allocator;

static int modelType;

struct Object
{
    cv::Rect_<float> rect;
    int label;
    float prob;
};

static inline float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

    #pragma omp parallel sections
    {
        #pragma omp section
        {
            if (left < j) qsort_descent_inplace(faceobjects, left, j);
        }
        #pragma omp section
        {
            if (i < right) qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects)
{
    if (faceobjects.empty())
        return;

    qsort_descent_inplace(faceobjects, 0, faceobjects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = faceobjects[i].rect.area();
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static inline float sigmoid(float x)
{
    return static_cast<float>(1.f / (1.f + exp(-x)));
}

static void generate_proposals(const ncnn::Mat& anchors, int stride, const ncnn::Mat& in_pad, const ncnn::Mat& feat_blob, float prob_threshold, std::vector<Object>& objects)
{
    const int num_grid_x = feat_blob.w;
    const int num_grid_y = feat_blob.h;

    const int num_anchors = anchors.w / 2;

    //?????????????????????
    int num_class = 7;
    int no = 12;
    if (modelType == 1) {
        //??????????????????????????????
        num_class = 2;
        no = 7;
    }

    for (int q = 0; q < num_anchors; q++)
    {
        const float anchor_w = anchors[q * 2];
        const float anchor_h = anchors[q * 2 + 1];

        for (int i = 0; i < num_grid_y; i++)
        {
            for (int j = 0; j < num_grid_x; j++)
            {
                // find class index with max class score
                int class_index = 0;
                float class_score = -FLT_MAX;
                for (int k = 0; k < num_class; k++)
                {
                    float score = feat_blob.channel(q * no + 5 + k).row(i)[j];
                    if (score > class_score)
                    {
                        class_index = k;
                        class_score = score;
                    }
                }

                float box_score = feat_blob.channel(q * no + 4).row(i)[j];

                float confidence = sigmoid(box_score) * sigmoid(class_score);

                if (confidence >= prob_threshold)
                {
                    // yolov5/models/yolo.py Detect forward
                    // y = x[i].sigmoid()
                    // y[..., 0:2] = (y[..., 0:2] * 2. - 0.5 + self.grid[i].to(x[i].device)) * self.stride[i]  # xy
                    // y[..., 2:4] = (y[..., 2:4] * 2) ** 2 * self.anchor_grid[i]  # wh

                    float dx = sigmoid(feat_blob.channel(q * no + 0).row(i)[j]);
                    float dy = sigmoid(feat_blob.channel(q * no + 1).row(i)[j]);
                    float dw = sigmoid(feat_blob.channel(q * no + 2).row(i)[j]);
                    float dh = sigmoid(feat_blob.channel(q * no + 3).row(i)[j]);

                    float pb_cx = (dx * 2.f - 0.5f + j) * stride;
                    float pb_cy = (dy * 2.f - 0.5f + i) * stride;

                    float pb_w = pow(dw * 2.f, 2) * anchor_w;
                    float pb_h = pow(dh * 2.f, 2) * anchor_h;

                    float x0 = pb_cx - pb_w * 0.5f;
                    float y0 = pb_cy - pb_h * 0.5f;
                    float x1 = pb_cx + pb_w * 0.5f;
                    float y1 = pb_cy + pb_h * 0.5f;

                    Object obj;
                    obj.rect.x = x0;
                    obj.rect.y = y0;
                    obj.rect.width = x1 - x0;
                    obj.rect.height = y1 - y0;
                    obj.label = class_index;
                    obj.prob = confidence;

                    objects.push_back(obj);

                }
            }
        }
    }
}

static int detect_yolov5(const cv::Mat& bgr, std::vector<Object>& objects)
{
    ncnn::Net yolov5;

    yolov5.opt.use_vulkan_compute = true;
    // yolov5.opt.use_bf16_storage = true;

    // original pretrained model from https://github.com/ultralytics/yolov5
    // the ncnn model https://github.com/nihui/ncnn-assets/tree/master/models
    yolov5.load_param("yolov5s.ncnn.param");
    yolov5.load_model("yolov5s.ncnn.bin");

    const int target_size = 640;
    const float prob_threshold = 0.25f;
    const float nms_threshold = 0.45f;

    int img_w = bgr.cols;
    int img_h = bgr.rows;

    // yolov5/models/common.py DetectMultiBackend
    const int max_stride = 64;

    // letterbox pad to multiple of max_stride
    int w = img_w;
    int h = img_h;
    float scale = 1.f;
    if (w > h)
    {
        scale = (float)target_size / w;
        w = target_size;
        h = h * scale;
    }
    else
    {
        scale = (float)target_size / h;
        h = target_size;
        w = w * scale;
    }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data, ncnn::Mat::PIXEL_BGR2RGB, img_w, img_h, w, h);

    // pad to target_size rectangle
    // yolov5/utils/datasets.py letterbox
    int wpad = (w + max_stride - 1) / max_stride * max_stride - w;
    int hpad = (h + max_stride - 1) / max_stride * max_stride - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 114.f);

    const float norm_vals[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
    in_pad.substract_mean_normalize(0, norm_vals);

    ncnn::Extractor ex = yolov5.create_extractor();

    ex.input("in0", in_pad);

    std::vector<Object> proposals;

    // anchor setting from yolov5/models/yolov5s.yaml

    // stride 8
    {
        ncnn::Mat out;
        ex.extract("out0", out);

        ncnn::Mat anchors(6);
        anchors[0] = 10.f;
        anchors[1] = 13.f;
        anchors[2] = 16.f;
        anchors[3] = 30.f;
        anchors[4] = 33.f;
        anchors[5] = 23.f;

        std::vector<Object> objects8;
        generate_proposals(anchors, 8, in_pad, out, prob_threshold, objects8);

        proposals.insert(proposals.end(), objects8.begin(), objects8.end());
    }

    // stride 16
    {
        ncnn::Mat out;
        ex.extract("out1", out);

        ncnn::Mat anchors(6);
        anchors[0] = 30.f;
        anchors[1] = 61.f;
        anchors[2] = 62.f;
        anchors[3] = 45.f;
        anchors[4] = 59.f;
        anchors[5] = 119.f;

        std::vector<Object> objects16;
        generate_proposals(anchors, 16, in_pad, out, prob_threshold, objects16);

        proposals.insert(proposals.end(), objects16.begin(), objects16.end());
    }

    // stride 32
    {
        ncnn::Mat out;
        ex.extract("out2", out);

        ncnn::Mat anchors(6);
        anchors[0] = 116.f;
        anchors[1] = 90.f;
        anchors[2] = 156.f;
        anchors[3] = 198.f;
        anchors[4] = 373.f;
        anchors[5] = 326.f;

        std::vector<Object> objects32;
        generate_proposals(anchors, 32, in_pad, out, prob_threshold, objects32);

        proposals.insert(proposals.end(), objects32.begin(), objects32.end());
    }

    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();

    objects.resize(count);
    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x - (wpad / 2)) / scale;
        float y0 = (objects[i].rect.y - (hpad / 2)) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width - (wpad / 2)) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height - (hpad / 2)) / scale;

        // clip
        x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;
    }

    return 0;
}

static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects)
{
    static const char* class_names[] = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"
    };

    cv::Mat image = bgr.clone();

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

        fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
                obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        cv::rectangle(image, obj.rect, cv::Scalar(255, 0, 0));

        char text[256];
        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > image.cols)
            x = image.cols - label_size.width;

        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                      cv::Scalar(255, 255, 255), -1);

        cv::putText(image, text, cv::Point(x, y + label_size.height),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
    }

    cv::imshow("image", image);
    cv::waitKey(0);
}

extern "C" {

// FIXME DeleteGlobalRef is missing for objCls
static jclass objCls = NULL;
static jmethodID constructortorId;
static jfieldID xId;
static jfieldID yId;
static jfieldID wId;
static jfieldID hId;
static jfieldID labelId;
static jfieldID probId;

char* Jstring2CStr(JNIEnv* env, jstring jstr) {
    char* rtn = NULL;
    jclass clsstring = env ->FindClass("java/lang/String");
    jstring strencode = env->NewStringUTF("GB2312");
    jmethodID mid = env->GetMethodID(clsstring, "getBytes", "(Ljava/lang/String;)[B");
    jbyteArray barr = (jbyteArray)env->CallObjectMethod(jstr, mid, strencode);
    jsize alen = env->GetArrayLength(barr);
    jbyte *ba = env->GetByteArrayElements(barr, JNI_FALSE);
    if (alen > 0) {
        rtn = (char*) malloc(alen+1);
        memcpy(rtn, ba, alen);
        rtn[alen] = 0;
    }
    env->ReleaseByteArrayElements(barr, ba, 0);
    return rtn;
}

char** stringArr2CharArr(JNIEnv* env, jobjectArray strArray) {
    jstring jstr;
    jsize len = env->GetArrayLength(strArray);
    char **pstr = (char **) malloc(len*sizeof(char *));
    for (int i = 0; i < len; i++) {
        jstr = (jstring) env->GetObjectArrayElement(strArray, i);
        pstr[i] = (char *)(*env).GetStringUTFChars(jstr, 0);
    }
    return pstr;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "JNI_OnLoad");

    ncnn::create_gpu_instance();

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "JNI_OnUnload");

    ncnn::destroy_gpu_instance();
}

// public native boolean Init(AssetManager mgr);
JNIEXPORT jboolean JNICALL Java_com_tencent_yolov5ncnn_YoloV5Ncnn_Init(JNIEnv* env, jobject thiz, jobject assetManager)
{

    modelType = 0;

    ncnn::Option opt;
    opt.lightmode = true;
    opt.num_threads = 4;
    opt.blob_allocator = &g_blob_pool_allocator;
    opt.workspace_allocator = &g_workspace_pool_allocator;
    opt.use_packing_layout = true;
    // use vulkan compute
    if (ncnn::get_gpu_count() != 0)
        opt.use_vulkan_compute = true;
    // yolov5.opt.use_bf16_storage = true;

    yolov5.opt = opt;

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    double start_time = ncnn::get_current_time();

    // init param
    {
        int ret = yolov5.load_param(mgr, "best.ncnn.param");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "load_param failed");
            return JNI_FALSE;
        }
    }

    // init bin
    {
        int ret = yolov5.load_model(mgr, "best.ncnn.bin");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "load_model failed");
            return JNI_FALSE;
        }
    }

    double elasped = ncnn::get_current_time() - start_time;
    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "%.2fms   load_model", elasped);

    // init jni glue
    jclass localObjCls = env->FindClass("com/tencent/yolov5ncnn/YoloV5Ncnn$Obj");
    objCls = reinterpret_cast<jclass>(env->NewGlobalRef(localObjCls));

    constructortorId = env->GetMethodID(objCls, "<init>", "(Lcom/tencent/yolov5ncnn/YoloV5Ncnn;)V");

    xId = env->GetFieldID(objCls, "x", "F");
    yId = env->GetFieldID(objCls, "y", "F");
    wId = env->GetFieldID(objCls, "w", "F");
    hId = env->GetFieldID(objCls, "h", "F");
    labelId = env->GetFieldID(objCls, "label", "Ljava/lang/String;");
    probId = env->GetFieldID(objCls, "prob", "F");

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_yolov5ncnn_YoloV5Ncnn_loadHandModel(JNIEnv* env, jobject thiz, jobject assetManager)
{

    modelType = 1;

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    double start_time = ncnn::get_current_time();

    // init param
    {
        int ret = yolov5.load_param(mgr, "hands_best.ncnn.param");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "load_param failed");
            return JNI_FALSE;
        }
    }

    // init bin
    {
        int ret = yolov5.load_model(mgr, "hands_best.ncnn.bin");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "load_model failed");
            return JNI_FALSE;
        }
    }

    double elasped = ncnn::get_current_time() - start_time;
    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "%.2fms   load_model", elasped);

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_com_tencent_yolov5ncnn_YoloV5Ncnn_loadMouthModel(JNIEnv* env, jobject thiz, jobject assetManager)
{

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    double start_time = ncnn::get_current_time();

    // init param
    {
        int ret = yolov5.load_param(mgr, "mouth_best.ncnn.param");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "load_param failed");
            return JNI_FALSE;
        }
    }

    // init bin
    {
        int ret = yolov5.load_model(mgr, "mouth_best.ncnn.bin");
        if (ret != 0)
        {
            __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "load_model failed");
            return JNI_FALSE;
        }
    }

    double elasped = ncnn::get_current_time() - start_time;
    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "%.2fms   load_model", elasped);

    return JNI_TRUE;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s [imagepath]\n", argv[0]);
        return -1;
    }

    const char* imagepath = argv[1];

    cv::Mat m = cv::imread(imagepath, 1);
    if (m.empty())
    {
        fprintf(stderr, "cv::imread %s failed\n", imagepath);
        return -1;
    }

    std::vector<Object> objects;
    detect_yolov5(m, objects);

    draw_objects(m, objects);

    return 0;
}

// public native Obj[] Detect(Bitmap bitmap, boolean use_gpu);
JNIEXPORT jobjectArray JNICALL Java_com_tencent_yolov5ncnn_YoloV5Ncnn_Detect(JNIEnv* env, jobject thiz, jobject bitmap, jboolean use_gpu)
{
    if (use_gpu == JNI_TRUE && ncnn::get_gpu_count() == 0)
    {
        return NULL;
        //return env->NewStringUTF("no vulkan capable gpu");
    }

    double start_time = ncnn::get_current_time();

    AndroidBitmapInfo info;
    AndroidBitmap_getInfo(env, bitmap, &info);
    const int width = info.width;
    const int height = info.height;
//    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "width=%d, height=%d", width, height);
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
        return NULL;

    // ncnn from bitmap
    const int target_size = 640;
    const float prob_threshold = 0.50f;
    const float nms_threshold = 0.45f;

    // yolov5/models/common.py DetectMultiBackend
    const int max_stride = 64;

    // letterbox pad to multiple of 32
    int w = width;
    int h = height;
    float scale = 1.f;
    if (w > h)
    {
        scale = (float)target_size / w;
        w = target_size;
        h = h * scale;
    }
    else
    {
        scale = (float)target_size / h;
        h = target_size;
        w = w * scale;
    }

    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "scaleWidth=%d, scaleHeight=%d", w, h);

    ncnn::Mat in = ncnn::Mat::from_android_bitmap_resize(env, bitmap, ncnn::Mat::PIXEL_RGB, w, h);

    // pad to target_size rectangle
    // yolov5/utils/datasets.py letterbox
    int wpad = (w + max_stride - 1) / max_stride * max_stride - w;
    int hpad = (h + max_stride - 1) / max_stride * max_stride - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 114.f);

    const float norm_vals[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
    in_pad.substract_mean_normalize(0, norm_vals);

    ncnn::Extractor ex = yolov5.create_extractor();
    //use gpu
    ex.set_vulkan_compute(use_gpu);

    ex.input("in0", in_pad);

    std::vector<Object> proposals;

    // anchor setting from yolov5/models/yolov5s.yaml


    // stride 8
    {
        ncnn::Mat out;
        ex.extract("out0", out);

        ncnn::Mat anchors(6);
        anchors[0] = 10.f;
        anchors[1] = 13.f;
        anchors[2] = 16.f;
        anchors[3] = 30.f;
        anchors[4] = 33.f;
        anchors[5] = 23.f;

        std::vector<Object> objects8;
        generate_proposals(anchors, 8, in_pad, out, prob_threshold, objects8);

        proposals.insert(proposals.end(), objects8.begin(), objects8.end());

    }

    // stride 16
    {
        ncnn::Mat out;
        ex.extract("out1", out);

        ncnn::Mat anchors(6);
        anchors[0] = 30.f;
        anchors[1] = 61.f;
        anchors[2] = 62.f;
        anchors[3] = 45.f;
        anchors[4] = 59.f;
        anchors[5] = 119.f;

        std::vector<Object> objects16;
        generate_proposals(anchors, 16, in_pad, out, prob_threshold, objects16);

        proposals.insert(proposals.end(), objects16.begin(), objects16.end());

    }

    // stride 32
    {
        ncnn::Mat out;
        ex.extract("out2", out);

        ncnn::Mat anchors(6);
        anchors[0] = 116.f;
        anchors[1] = 90.f;
        anchors[2] = 156.f;
        anchors[3] = 198.f;
        anchors[4] = 373.f;
        anchors[5] = 326.f;

        std::vector<Object> objects32;
        generate_proposals(anchors, 32, in_pad, out, prob_threshold, objects32);

        proposals.insert(proposals.end(), objects32.begin(), objects32.end());

    }


    // sort all proposals by score from highest to lowest
    qsort_descent_inplace(proposals);

    // apply nms with nms_threshold
    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, nms_threshold);

    int count = picked.size();

    // yolov5
    std::vector<Object> objects;
    objects.resize(count);
    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x - (wpad / 2)) / scale;
        float y0 = (objects[i].rect.y - (hpad / 2)) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width - (wpad / 2)) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height - (hpad / 2)) / scale;

        // clip
        x0 = std::max(std::min(x0, (float)(width - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(height - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(width - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(height - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;
    }

    // objects to Obj[]
//    static const char* class_names[] = {
//        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
//        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
//        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
//        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
//        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
//        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
//        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
//        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
//        "hair drier", "toothbrush"
//    };


    jobjectArray jObjArray = env->NewObjectArray(objects.size(), objCls, NULL);

    if (modelType == 1) {
        static const char* class_names[] = {
            "open mouth", "push hands"
        };
        for (size_t i=0; i<objects.size(); i++)
        {
            jobject jObj = env->NewObject(objCls, constructortorId, thiz);

            env->SetFloatField(jObj, xId, objects[i].rect.x);
            env->SetFloatField(jObj, yId, objects[i].rect.y);
            env->SetFloatField(jObj, wId, objects[i].rect.width);
            env->SetFloatField(jObj, hId, objects[i].rect.height);
            env->SetObjectField(jObj, labelId, env->NewStringUTF(class_names[objects[i].label]));
            env->SetFloatField(jObj, probId, objects[i].prob);

            env->SetObjectArrayElement(jObjArray, i, jObj);
        }
    } else {
        static const char* class_names[] = {
            "abnormal oral", "oral herpes", "hand scars", "hand rash", "facial scars", "red eye", "tooth decay"
        };
        for (size_t i=0; i<objects.size(); i++)
        {
            jobject jObj = env->NewObject(objCls, constructortorId, thiz);

            env->SetFloatField(jObj, xId, objects[i].rect.x);
            env->SetFloatField(jObj, yId, objects[i].rect.y);
            env->SetFloatField(jObj, wId, objects[i].rect.width);
            env->SetFloatField(jObj, hId, objects[i].rect.height);
            env->SetObjectField(jObj, labelId, env->NewStringUTF(class_names[objects[i].label]));
            env->SetFloatField(jObj, probId, objects[i].prob);

            env->SetObjectArrayElement(jObjArray, i, jObj);
        }
    }


    double elasped = ncnn::get_current_time() - start_time;
    __android_log_print(ANDROID_LOG_DEBUG, "YoloV5Ncnn", "%.2fms   detect", elasped);

    return jObjArray;
}

}