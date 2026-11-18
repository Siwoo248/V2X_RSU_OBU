#include "v2x_protocol.h"
#include <termios.h>
#include <signal.h>

// 차량 상태 정보
char my_vehicle_id[ID_LEN] = "CAR01";
double my_pos_x = 100.0;
double my_pos_y = 50.0;
double my_speed = 60.0; // km/h
int sock = -1;
volatile int running = 1;
int is_accident = 0;

pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
struct termios orig_termios;

// 터미널 원상 복구 함수
void reset_terminal_mode() {
    if (isatty(STDIN_FILENO)) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    }
}

// 종료 시그널 핸들러
void handle_sigint(int sig) {
    (void)sig;
    running = 0;
    reset_terminal_mode();
    if (sock >= 0) close(sock);
    printf("\n[OBU] 프로그램을 종료합니다.\n");
    exit(0);
}

// 2단계: 1초마다 위치와 속도(BSM)를 전송하는 스레드
void* bsm_thread(void *arg) {
    (void)arg;
    V2XPacket pkt;

    while (running) {
        sleep(1);

        pthread_mutex_lock(&state_lock);
        // 사고가 나지 않았다면 위치 이동 시뮬레이션
        if (!is_accident) {
            // 속도(km/h) -> m/s 변환 후 1초 동안 이동
            my_pos_x += my_speed * (1000.0 / 3600.0);
            // 약간의 속도 변동 (58 ~ 62 km/h)
            my_speed = 60.0 + ((rand() % 5) - 2);
        } else {
            my_speed = 0.0; // 사고 시 정차
        }

        memset(&pkt, 0, sizeof(pkt));
        pkt.msg_type = MSG_TYPE_BSM;
        snprintf(pkt.vehicle_id, sizeof(pkt.vehicle_id), "%s", my_vehicle_id);
        pkt.pos_x = my_pos_x;
        pkt.pos_y = my_pos_y;
        pkt.speed = my_speed;
        pkt.timestamp = time(NULL);
        pthread_mutex_unlock(&state_lock);

        if (send_packet(sock, &pkt) < 0) {
            printf(COLOR_RED "[OBU] RSU 서버와의 연결이 끊어졌습니다.\n" COLOR_RESET);
            running = 0;
            break;
        }

        printf(COLOR_GREEN "[BSM 송신] %s | 위치: (%6.1f, %6.1f) | 속도: %5.1f km/h\n" COLOR_RESET,
               pkt.vehicle_id, pkt.pos_x, pkt.pos_y, pkt.speed);
    }
    return NULL;
}

// RSU 및 타 차량 메시지를 수신하는 스레드
void* receiver_thread(void *arg) {
    (void)arg;
    V2XPacket pkt;

    while (running) {
        int res = recv_packet(sock, &pkt);
        if (res <= 0) {
            if (running) {
                printf(COLOR_RED "\n[OBU] 서버로부터 연결이 종료되었습니다.\n" COLOR_RESET);
                running = 0;
            }
            break;
        }

        if (pkt.msg_type == MSG_TYPE_BSM) {
            // 2단계: 주변 차량 BSM 수신
            printf(COLOR_CYAN "  └─ [주변 BSM 수신] 차량: %-6s | 위치: (%6.1f, %6.1f) | 속도: %5.1f km/h\n" COLOR_RESET,
                   pkt.vehicle_id, pkt.pos_x, pkt.pos_y, pkt.speed);
        }
        else if (pkt.msg_type == MSG_TYPE_DENM) {
            // 3단계: 주변 사고 경고(DENM) 수신 -> 경고 출력 및 서행 감속
            pthread_mutex_lock(&state_lock);
            if (!is_accident && my_speed > 30.0) {
                my_speed = 30.0; // 전방 사고 인지 후 30km/h로 감속
            }
            pthread_mutex_unlock(&state_lock);

            printf("\n" COLOR_RED COLOR_BOLD);
            printf("****************************************************************\n");
            printf("🚨 [긴급 경고 - DENM 수신!] 전방 사고 발생 경보!\n");
            printf("   - 사고 차량 : %s\n", pkt.vehicle_id);
            printf("   - 사고 위치 : (%.1f, %.1f)\n", pkt.pos_x, pkt.pos_y);
            printf("   - 상세 내용 : %s\n", pkt.alert_desc);
            printf("   ⚠️ 전방 사고 감지! 30km/h로 감속 및 비상등을 켭니다.\n");
            printf("****************************************************************\n");
            printf(COLOR_RESET "\n");
        }
    }
    return NULL;
}

// 3단계: 사고 알림(DENM) 전송 함수
void trigger_accident() {
    pthread_mutex_lock(&state_lock);
    is_accident = 1;
    my_speed = 0.0; // 충돌로 인한 정지

    V2XPacket denm_pkt;
    memset(&denm_pkt, 0, sizeof(denm_pkt));
    denm_pkt.msg_type = MSG_TYPE_DENM;
    snprintf(denm_pkt.vehicle_id, sizeof(denm_pkt.vehicle_id), "%s", my_vehicle_id);
    denm_pkt.pos_x = my_pos_x;
    denm_pkt.pos_y = my_pos_y;
    denm_pkt.speed = 0.0;
    snprintf(denm_pkt.alert_desc, sizeof(denm_pkt.alert_desc), "%s", "전방 추돌 사고 발생! 긴급 정차!");
    denm_pkt.timestamp = time(NULL);
    pthread_mutex_unlock(&state_lock);

    if (send_packet(sock, &denm_pkt) < 0) {
        printf(COLOR_RED "[OBU] DENM 전송 실패!\n" COLOR_RESET);
    } else {
        printf("\n" COLOR_RED COLOR_BOLD);
        printf("💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥\n");
        printf("💥 [사고 발생!] 내 차량(%s) 사고 알림(DENM)을 RSU로 전송했습니다!\n", my_vehicle_id);
        printf("   - 사고 위치 : (%.1f, %.1f)\n", denm_pkt.pos_x, denm_pkt.pos_y);
        printf("   - 현재 상태 : 정차 (속도: 0 km/h)\n");
        printf("💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥💥\n");
        printf(COLOR_RESET "\n");
    }
}

int main(int argc, char *argv[]) {
    setbuf(stdout, NULL);
    srand(time(NULL));

    // 차량 ID 설정 (인자로 받거나 입력받기)
    if (argc >= 2) {
        strncpy(my_vehicle_id, argv[1], ID_LEN - 1);
        my_vehicle_id[ID_LEN - 1] = '\0';
    } else {
        printf("차량 ID를 입력하세요 (예: CAR01, CAR02): ");
        if (scanf("%15s", my_vehicle_id) != 1) {
            strcpy(my_vehicle_id, "CAR01");
        }
        // 개행 문자 비우기
        while (getchar() != '\n');
    }

    // 차량 ID에 따라 초기 위치 및 속도 차등 부여
    if (strcmp(my_vehicle_id, "CAR01") == 0) {
        my_pos_x = 100.0;
        my_pos_y = 50.0;
        my_speed = 60.0;
    } else if (strcmp(my_vehicle_id, "CAR02") == 0) {
        my_pos_x = 250.0;
        my_pos_y = 50.0;
        my_speed = 65.0;
    } else {
        my_pos_x = (rand() % 200) + 50.0;
        my_pos_y = 50.0;
        my_speed = 60.0;
    }

    signal(SIGINT, handle_sigint);

    // 소켓 생성
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("소켓 생성 실패");
        exit(1);
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("유효하지 않은 주소");
        exit(1);
    }

    printf("[OBU] RSU 서버(%s:%d)로 접속 시도 중...\n", SERVER_IP, SERVER_PORT);
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf(COLOR_RED "❌ RSU 서버 접속 실패! (RSU 서버가 먼저 켜져 있는지 확인하세요)\n" COLOR_RESET);
        exit(1);
    }

    // 1단계: RSU에 차량 ID 등록 요청
    V2XPacket reg_pkt;
    memset(&reg_pkt, 0, sizeof(reg_pkt));
    reg_pkt.msg_type = MSG_TYPE_REGISTER;
    snprintf(reg_pkt.vehicle_id, sizeof(reg_pkt.vehicle_id), "%s", my_vehicle_id);
    reg_pkt.pos_x = my_pos_x;
    reg_pkt.pos_y = my_pos_y;
    reg_pkt.speed = my_speed;
    reg_pkt.timestamp = time(NULL);

    if (send_packet(sock, &reg_pkt) < 0) {
        perror("등록 패킷 전송 실패");
        exit(1);
    }

    // ACK 수신 대기
    V2XPacket ack_pkt;
    if (recv_packet(sock, &ack_pkt) <= 0 || ack_pkt.msg_type != MSG_TYPE_ACK) {
        printf(COLOR_RED "❌ RSU 등록 확인(ACK) 수신 실패!\n" COLOR_RESET);
        exit(1);
    }

    printf(COLOR_GREEN COLOR_BOLD "========================================================\n");
    printf("   🚗 [1단계 접속 완료] 차량 ID 등록 성공: %s\n", my_vehicle_id);
    printf("   - 초기 위치: (%.1f, %.1f) | 속도: %.1f km/h\n", my_pos_x, my_pos_y, my_speed);
    printf("========================================================\n" COLOR_RESET);

    // 스레드 시작 (BSM 주기 송신 스레드, 수신 스레드)
    pthread_t tid_bsm, tid_recv;
    pthread_create(&tid_bsm, NULL, bsm_thread, NULL);
    pthread_create(&tid_recv, NULL, receiver_thread, NULL);
    pthread_detach(tid_bsm);
    pthread_detach(tid_recv);

    printf("\n" COLOR_YELLOW COLOR_BOLD);
    printf("--------------------------------------------------------\n");
    printf(" 💡 [3단계 조작 안내]\n");
    printf("    키보드에서 'a' 키 (또는 Enter 키)를 누르면\n");
    printf("    즉시 사고 알림(DENM)이 RSU 및 다른 차량으로 전송됩니다!\n");
    printf("    (종료: 'q' 키 또는 Ctrl + C)\n");
    printf("--------------------------------------------------------\n" COLOR_RESET "\n");

    // 단일 키 입력을 위한 non-canonical 모드 설정 (터미널인 경우에만 적용)
    if (isatty(STDIN_FILENO)) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        atexit(reset_terminal_mode);
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO); // 버퍼링 및 화면 에코 끄기
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    // 키 입력 감지 루프
    while (running) {
        int c = getchar();
        if (c == EOF) {
            usleep(100000); // 0.1초 대기 후 재확인
            continue;
        }
        if (c == 'q' || c == 'Q') {
            break;
        } else if (c == 'a' || c == 'A' || c == '\n' || c == ' ') {
            // 키 하나로 사고 알림 전송!
            trigger_accident();
        }
    }

    running = 0;
    reset_terminal_mode();
    close(sock);
    printf("\n[OBU] %s 종료되었습니다.\n", my_vehicle_id);
    return 0;
}
