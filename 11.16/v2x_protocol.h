#ifndef V2X_PROTOCOL_H
#define V2X_PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9000
#define MAX_VEHICLES 10
#define ID_LEN 16
#define DESC_LEN 64

// ANSI 컬러 코드 (가독성을 위한 콘솔 출력 색상)
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[1;31m"
#define COLOR_GREEN   "\033[1;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[1;34m"
#define COLOR_MAGENTA "\033[1;35m"
#define COLOR_CYAN    "\033[1;36m"
#define COLOR_BOLD    "\033[1m"

// 메시지 타입 정의
typedef enum {
    MSG_TYPE_REGISTER = 1, // 1단계: RSU 접속 및 차량 ID 등록
    MSG_TYPE_BSM      = 2, // 2단계: Basic Safety Message (1초 주기 상태 전송)
    MSG_TYPE_DENM     = 3, // 3단계: Decentralized Environmental Notification (사고 알림)
    MSG_TYPE_ACK      = 4  // 등록 확인 응답
} MsgType;

// V2X 패킷 구조체
typedef struct {
    int msg_type;              // MsgType
    char vehicle_id[ID_LEN];   // 차량 ID (예: "CAR01", "CAR02")
    double pos_x;              // 차량 위치 X좌표 (m)
    double pos_y;              // 차량 위치 Y좌표 (m)
    double speed;              // 차량 속도 (km/h)
    char alert_desc[DESC_LEN]; // 사고/경고 내용 (DENM용)
    long timestamp;            // 타임스탬프 (초)
} V2XPacket;

// TCP 스트림 상에서 정확한 크기만큼 송신하는 헬퍼 함수 (성공 시 1, 실패 시 -1)
static inline int send_packet(int sock, const V2XPacket *pkt) {
    size_t total_sent = 0;
    const char *ptr = (const char *)pkt;
    while (total_sent < sizeof(V2XPacket)) {
        ssize_t sent = send(sock, ptr + total_sent, sizeof(V2XPacket) - total_sent, 0);
        if (sent <= 0) return -1;
        total_sent += sent;
    }
    return 1;
}

// TCP 스트림 상에서 정확한 크기만큼 수신하는 헬퍼 함수 (성공 시 1, 연결 종료 시 0, 오류 시 -1)
static inline int recv_packet(int sock, V2XPacket *pkt) {
    size_t total_recv = 0;
    char *ptr = (char *)pkt;
    while (total_recv < sizeof(V2XPacket)) {
        ssize_t recvd = recv(sock, ptr + total_recv, sizeof(V2XPacket) - total_recv, 0);
        if (recvd == 0) return 0;  // 정상 연결 종료
        if (recvd < 0) return -1;  // 소켓 오류
        total_recv += recvd;
    }
    return 1; // 1개 패킷 온전히 수신 완료
}

#endif // V2X_PROTOCOL_H
