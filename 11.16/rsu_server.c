#include "v2x_protocol.h"

typedef struct {
    int sock;
    char vehicle_id[ID_LEN];
    int active;
    struct sockaddr_in addr;
} Client;

Client clients[MAX_VEHICLES];
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

// 현재 등록된 활성 차량 수 반환
int get_active_client_count() {
    int count = 0;
    for (int i = 0; i < MAX_VEHICLES; i++) {
        if (clients[i].active) count++;
    }
    return count;
}

// 송신자(sender_sock)를 제외한 모든 연결된 차량에 브로드캐스트 전송
void broadcast_packet(const V2XPacket *pkt, int sender_sock) {
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_VEHICLES; i++) {
        if (clients[i].active && clients[i].sock != sender_sock) {
            if (send_packet(clients[i].sock, pkt) < 0) {
                // 송신 실패 시 연결 종료 처리
                printf(COLOR_RED "[RSU] %s 차량으로 패킷 전송 실패\n" COLOR_RESET, clients[i].vehicle_id);
            }
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

// 각 클라이언트(차량 OBU)를 전담 처리하는 스레드 함수
void* client_handler(void *arg) {
    int client_sock = *(int*)arg;
    free(arg);

    int client_index = -1;
    char client_id[ID_LEN] = "UNKNOWN";
    V2XPacket pkt;

    // 1단계: 최초 접속 후 차량 ID 등록 대기
    if (recv_packet(client_sock, &pkt) <= 0 || pkt.msg_type != MSG_TYPE_REGISTER) {
        printf(COLOR_RED "[RSU] 차량 ID 등록 패킷 수신 실패. 연결을 종료합니다.\n" COLOR_RESET);
        close(client_sock);
        return NULL;
    }

    // 클라이언트 목록에 등록
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_VEHICLES; i++) {
        if (!clients[i].active) {
            clients[i].sock = client_sock;
            snprintf(clients[i].vehicle_id, sizeof(clients[i].vehicle_id), "%s", pkt.vehicle_id);
            clients[i].active = 1;
            client_index = i;
            snprintf(client_id, sizeof(client_id), "%s", clients[i].vehicle_id);
            break;
        }
    }
    int total_connected = get_active_client_count();
    pthread_mutex_unlock(&clients_lock);

    if (client_index == -1) {
        printf(COLOR_RED "[RSU] 최대 접속 차량 한도 초과! 접속을 거절합니다.\n" COLOR_RESET);
        close(client_sock);
        return NULL;
    }

    // 1단계 등록 완료: ACK 전송
    V2XPacket ack_pkt;
    memset(&ack_pkt, 0, sizeof(ack_pkt));
    ack_pkt.msg_type = MSG_TYPE_ACK;
    strncpy(ack_pkt.vehicle_id, "RSU_SERVER", ID_LEN - 1);
    strcpy(ack_pkt.alert_desc, "등록 성공");
    ack_pkt.timestamp = time(NULL);
    send_packet(client_sock, &ack_pkt);

    printf(COLOR_GREEN "[RSU] 🚗 [1단계 차량 등록] %s 차량 접속 완료! (현재 접속: %d대)\n" COLOR_RESET,
           client_id, total_connected);

    // 메시지 수신 루프
    while (1) {
        int res = recv_packet(client_sock, &pkt);
        if (res <= 0) {
            // 클라이언트 연결 종료
            break;
        }

        if (pkt.msg_type == MSG_TYPE_BSM) {
            // 2단계: BSM 수신 및 주변 차량으로 중계(브로드캐스트)
            printf(COLOR_CYAN "[RSU - BSM 수신] 차량: %-6s | 위치: (%6.1f, %6.1f) | 속도: %5.1f km/h\n" COLOR_RESET,
                   pkt.vehicle_id, pkt.pos_x, pkt.pos_y, pkt.speed);

            // 다른 차량들에게 중계
            broadcast_packet(&pkt, client_sock);
        }
        else if (pkt.msg_type == MSG_TYPE_DENM) {
            // 3단계: DENM (사고 알림) 수신 및 긴급 전파
            printf("\n" COLOR_RED COLOR_BOLD);
            printf("====================================================================\n");
            printf("🚨 [RSU - DENM 사고 알림 수신] 긴급 상황 발생!\n");
            printf("   - 발생 차량 : %s\n", pkt.vehicle_id);
            printf("   - 사고 위치 : (%.1f, %.1f)\n", pkt.pos_x, pkt.pos_y);
            printf("   - 사고 내용 : %s\n", pkt.alert_desc);
            printf("📢 [RSU] 주변 전 차량에 사고 경보(DENM)를 브로드캐스트합니다!\n");
            printf("====================================================================\n");
            printf(COLOR_RESET "\n");

            // 다른 차량들에게 즉시 브로드캐스트
            broadcast_packet(&pkt, client_sock);
        }
    }

    // 연결 종료 처리
    pthread_mutex_lock(&clients_lock);
    if (client_index >= 0) {
        clients[client_index].active = 0;
        clients[client_index].sock = -1;
    }
    int remaining = get_active_client_count();
    pthread_mutex_unlock(&clients_lock);

    printf(COLOR_YELLOW "[RSU] 🚗 [차량 퇴장] %s 연결 종료 (현재 접속: %d대)\n" COLOR_RESET, client_id, remaining);
    close(client_sock);
    return NULL;
}

int main() {
    setbuf(stdout, NULL);
    int server_sock, client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int opt = 1;

    // 소켓 생성
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("소켓 생성 실패");
        exit(1);
    }

    // 주소 재사용 옵션 설정
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr.sin_port = htons(SERVER_PORT);

    // 바인드
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("바인드 실패");
        close(server_sock);
        exit(1);
    }

    // 리슨
    if (listen(server_sock, 10) < 0) {
        perror("리슨 실패");
        close(server_sock);
        exit(1);
    }

    printf(COLOR_BOLD "========================================================\n");
    printf("   📡 미니 V2X - RSU (Roadside Unit) 서버 시작\n");
    printf("   - 바인드 주소 : %s\n", SERVER_IP);
    printf("   - 포트        : %d\n", SERVER_PORT);
    printf("   - 차량 접속을 기다리는 중...\n");
    printf("========================================================\n" COLOR_RESET);

    // 클라이언트 접속 수락 루프
    while (1) {
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            perror("접속 수락 실패");
            continue;
        }

        int *new_sock = malloc(sizeof(int));
        *new_sock = client_sock;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, new_sock) != 0) {
            perror("스레드 생성 실패");
            close(client_sock);
            free(new_sock);
        } else {
            pthread_detach(tid);
        }
    }

    close(server_sock);
    return 0;
}
