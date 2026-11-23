#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define DEFAULT_IP               "127.0.0.1"
#define DEFAULT_PORT             9000
#define BUFFER_SIZE              2048
#define MAX_CLIENTS              32

// 차선 좌표 정의
#define LANE_1_Y                 100     // 1차로 (기본 주행 차선)
#define LANE_2_Y                 200     // 2차로 (추월 및 회피 차선)

// 고속 주행(60~70m/s) 안전 및 추월 기준 (m)
#define SAFE_DISTANCE            200     // 200m 이내 진입 시 긴급 차선 회피 발동
#define CRASH_DISTANCE           100     // 100m 이내에서 앞차 사고 시 제동 불가 -> 연쇄 충돌!
#define OVERTAKE_DISTANCE        50      // 평상시 앞차와 50m 이내로 붙으면 2차로 추월 시작
#define OVERTAKE_CLEAR_DISTANCE  50      // 앞차를 50m 이상 앞지르면 1차로 안전 복귀

// 24비트 TrueColor 픽셀 그래픽 해상도
#define PIXEL_WIDTH              72      // 터미널 가로 72픽셀
#define PIXEL_HEIGHT             12      // 터미널 세로 12픽셀 (터미널 6줄 출력)

typedef struct {
    unsigned char r;
    unsigned char g;
    unsigned char b;
} RGBColor;

static inline RGBColor make_rgb(unsigned char r, unsigned char g, unsigned char b) {
    RGBColor c = {r, g, b};
    return c;
}

// ANSI 색상 코드 정의
#define COLOR_RESET              "\033[0m"
#define COLOR_RED                "\033[1;31m"
#define COLOR_GREEN              "\033[1;32m"
#define COLOR_YELLOW             "\033[1;33m"
#define COLOR_BLUE               "\033[1;34m"
#define COLOR_MAGENTA            "\033[1;35m"
#define COLOR_CYAN               "\033[1;36m"
#define COLOR_WHITE              "\033[1;37m"
#define COLOR_BOLD               "\033[1m"
#define COLOR_GRAY               "\033[0;90m"

#endif // COMMON_H
