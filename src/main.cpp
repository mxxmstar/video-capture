/**
 * RV1126 MPP (Media Process Platform) 测试Demo
 * 
 * 功能说明：
 * 1. 初始化MPP环境
 * 2. 创建视频解码器实例
 * 3. 打印MPP支持的格式信息
 * 4. 演示基本的MPP API调用流程
 * 
 * 编译方法：
 *   mkdir build && cd build
 *   cmake ..
 *   make
 * 
 * 运行方法（在RV1126开发板上）：
 *   ./mpp_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MPP公共API头文件 - 使用官方公开接口 */
#include "rk_mpi.h"          /* MPP主接口 (MPI API) */
#include "rk_type.h"         /* 基本类型定义 */
#include "mpp_frame.h"       /* 视频帧数据结构 */
#include "mpp_packet.h"      /* 数据包结构 */
#include "mpp_buffer.h"      /* 内存缓冲管理 */
#include "mpp_err.h"         /* 错误码定义 */
#include "mpp_meta.h"        /* 元数据管理 */
#include "mpp_task.h"        /* 任务管理 */

/**
 * 打印MPP支持的编码格式
 * 用于验证MPP库是否正确链接
 */
void print_mpp_info(void)
{
    printf("===========================================\n");
    printf("MPP Information\n");
    printf("===========================================\n");
    
    /* 打印MPP支持的编码格式 */
    printf("Supported encoding formats:\n");
    mpp_show_support_format();
    
    printf("\n");
    printf("===========================================\n\n");
}

/**
 * 测试MPP解码器创建和销毁
 * 演示基本的MPP上下文生命周期管理
 * 
 * @param coding_type 编码格式 (MPP_VIDEO_CodingAVC, MPP_VIDEO_CodingHEVC等)
 * @return 0表示成功，-1表示失败
 */
int test_mpp_decoder(MppCodingType coding_type)
{
    MppCtx ctx = NULL;          /* MPP上下文句柄 */
    MppApi *mpi = NULL;         /* MPP API接口指针 */
    MPP_RET ret = MPP_OK;       /* MPP函数返回值 */
    
    const char *coding_name = NULL;
    
    /* 根据编码类型设置名称 */
    switch (coding_type) {
        case MPP_VIDEO_CodingAVC:
            coding_name = "H.264/AVC";
            break;
        case MPP_VIDEO_CodingHEVC:
            coding_name = "H.265/HEVC";
            break;
        case MPP_VIDEO_CodingMJPEG:
            coding_name = "MJPEG";
            break;
        default:
            coding_name = "Unknown";
            break;
    }
    
    printf("[Decoder Test] Testing %s decoder...\n", coding_name);
    
    /* 步骤1: 创建MPP上下文
     * mpp_create() 创建MPP ctx和mpi接口
     * ctx: 输出参数，MPP上下文句柄
     * mpi: 输出参数，MPP API接口
     */
    ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        printf("[ERROR] mpp_create failed with ret=%d\n", ret);
        return -1;
    }
    printf("[Decoder Test] mpp_create success\n");
    
    /* 步骤2: 初始化MPP上下文
     * mpp_init() 初始化MPP ctx
     * ctx: MPP上下文句柄
     * type: MPP_CTX_DEC(解码器), MPP_CTX_ENC(编码器), MPP_CTX_ISP(ISP)
     * coding: 编码格式
     */
    ret = mpp_init(ctx, MPP_CTX_DEC, coding_type);
    if (ret != MPP_OK) {
        printf("[ERROR] mpp_init failed with ret=%d\n", ret);
        mpp_destroy(ctx);  /* 失败时需要销毁已创建的ctx */
        return -1;
    }
    printf("[Decoder Test] mpp_init success (decoder mode)\n");
    
    /* 步骤3: 配置解码器参数（可选）
     * 这里可以设置解码器的各种参数
     * 例如：输出格式、缓冲区大小等
     */
    
    /* 步骤4: 销毁MPP上下文
     * 使用完毕后必须调用mpp_destroy()释放资源
     */
    ret = mpp_destroy(ctx);
    if (ret != MPP_OK) {
        printf("[ERROR] mpp_destroy failed with ret=%d\n", ret);
        return -1;
    }
    printf("[Decoder Test] mpp_destroy success\n");
    printf("[Decoder Test] %s decoder test passed!\n\n", coding_name);
    
    return 0;
}

/**
 * 测试MPP编码器创建和销毁
 * 演示编码器的基本使用流程
 * 
 * @param coding_type 编码格式
 * @return 0表示成功，-1表示失败
 */
int test_mpp_encoder(MppCodingType coding_type)
{
    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    MPP_RET ret = MPP_OK;
    
    const char *coding_name = NULL;
    
    switch (coding_type) {
        case MPP_VIDEO_CodingAVC:
            coding_name = "H.264/AVC";
            break;
        case MPP_VIDEO_CodingHEVC:
            coding_name = "H.265/HEVC";
            break;
        default:
            coding_name = "Unknown";
            break;
    }
    
    printf("[Encoder Test] Testing %s encoder...\n", coding_name);
    
    /* 创建MPP上下文 */
    ret = mpp_create(&ctx, &mpi);
    if (ret != MPP_OK) {
        printf("[ERROR] mpp_create failed with ret=%d\n", ret);
        return -1;
    }
    
    /* 初始化为编码器模式 */
    ret = mpp_init(ctx, MPP_CTX_ENC, coding_type);
    if (ret != MPP_OK) {
        printf("[ERROR] mpp_init failed with ret=%d\n", ret);
        mpp_destroy(ctx);
        return -1;
    }
    printf("[Encoder Test] mpp_init success (encoder mode)\n");
    
    /* 编码器配置示例（实际使用时需要配置分辨率、码率等参数）
     * MppEncCfg cfg;
     * mpp_enc_cfg_init(&cfg);
     * mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
     * mpp_enc_cfg_set_s32(cfg, "prep:width", 1920);
     * mpp_enc_cfg_set_s32(cfg, "prep:height", 1080);
     * mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
     */
    
    /* 销毁MPP上下文 */
    ret = mpp_destroy(ctx);
    if (ret != MPP_OK) {
        printf("[ERROR] mpp_destroy failed with ret=%d\n", ret);
        return -1;
    }
    printf("[Encoder Test] %s encoder test passed!\n\n", coding_name);
    
    return 0;
}

/**
 * 主函数 - 程序入口
 * 执行所有MPP测试
 */
int main(int argc, char **argv)
{
    int ret = 0;
    
    printf("\n");
    printf("############################################\n");
    printf("#                                          #\n");
    printf("#    RV1126 MPP Test Demo                  #\n");
    printf("#                                          #\n");
    printf("############################################\n\n");
    
    /* 1. 打印MPP支持的格式信息 */
    print_mpp_info();
    
    /* 2. 测试H.264解码器 */
    if (test_mpp_decoder(MPP_VIDEO_CodingAVC) != 0) {
        printf("[WARNING] H.264 decoder test failed\n");
        ret = -1;
    }
    
    /* 3. 测试H.265解码器 */
    if (test_mpp_decoder(MPP_VIDEO_CodingHEVC) != 0) {
        printf("[WARNING] H.265 decoder test failed\n");
        ret = -1;
    }
    
    /* 4. 测试H.264编码器 */
    if (test_mpp_encoder(MPP_VIDEO_CodingAVC) != 0) {
        printf("[WARNING] H.264 encoder test failed\n");
        ret = -1;
    }
    
    /* 5. 测试H.265编码器 */
    if (test_mpp_encoder(MPP_VIDEO_CodingHEVC) != 0) {
        printf("[WARNING] H.265 encoder test failed\n");
        ret = -1;
    }
    
    /* 打印测试结果 */
    printf("\n===========================================\n");
    if (ret == 0) {
        printf("All MPP tests passed!\n");
    } else {
        printf("Some MPP tests failed!\n");
    }
    printf("===========================================\n\n");
    
    return ret;
}