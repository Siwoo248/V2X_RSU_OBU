#include "common.h"
#include <termios.h>
#include <signal.h>
#include <strings.h>

typedef struct {
    char car_id[64];
    int x;
    int y;
    int speed;
    int is_accident;
    int running;
    int sock_fd;
    pthread_mutex_t lock;
} VehicleState;

static VehicleState g_car;
static struct termios orig_termios;
static int termios_modified = 0;

// 터미널 원래 모드로 복원
void reset_terminal_mode(void) {
    if (termios_modified) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        termios_modified = 0;
    }
}

// 터미널을 Non-canonical & Non-echo 모드로 전환 (엔터 없이 즉시 키 입력 감지)
void set_raw_terminal_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
        return;
    }
    termios_modified = 1;
    atexit(reset_terminal_mode);

    struct termios new_termios = orig_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

// 시그널 핸들러 (Ctrl+C 등 종료 시 터미널 복원)
void sigint_handler(int sig) {
    (void)sig;
    reset_terminal_mode();
    printf("\n" COLOR_YELLOW "[OBU] 차량 시스템을 종료합니다." COLOR_RESET "\n");
    exit(0);
}

// RSU 수신 전담 스레드 (다른 차 BSM, 사고 경고 수신)
void *recv_thread_func(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];
    char line_buf[BUFFER_SIZE];
    int line_len = 0;

    while (g_car.running) {
        ssize_t bytes = recv(g_car.sock_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            printf("\n" COLOR_RED "[OBU] RSU 서버와의 연결이 끊어졌습니다." COLOR_RESET "\n");
            g_car.running = 0;
            break;
        }

        for (ssize_t i = 0; i < bytes; i++) {
            char ch = buffer[i];
            if (ch == '\n') {
                line_buf[line_len] = '\0';
                if (line_len > 0) {
                    // 수신된 라인 메시지 파싱
                    char copy[BUFFER_SIZE];
                    strncpy(copy, line_buf, sizeof(copy) - 1);
                    copy[sizeof(copy) - 1] = '\0';

                    char *type = strtok(copy, "|");
                    if (type) {
                        // 1. 다른 차량의 사고 발생 경고 수신
                        if (strcmp(type, "WARNING") == 0) {
                            char *acc_id = strtok(NULL, "|");
                            char *acc_pos = strtok(NULL, "|");
                            if (acc_id && acc_pos) {
                                int acc_x = 0, acc_y = 0;
                                sscanf(acc_pos, "%d,%d", &acc_x, &acc_y);

                                pthread_mutex_lock(&g_car.lock);
                                int cur_x = g_car.x;
                                int cur_y = g_car.y;

                                if (acc_x > cur_x) {
                                    int dist = acc_x - cur_x;
                                    printf("\n" COLOR_RED COLOR_BOLD "======================================================================" COLOR_RESET "\n");
                                    printf(COLOR_RED COLOR_BOLD "🚨 [긴급 경고: 전방 사고 감지 & 충돌 위험!]                         " COLOR_RESET "\n");
                                    printf(COLOR_RED "  * 사고 차량   : %s\n" COLOR_RESET, acc_id);
                                    printf(COLOR_RED "  * 사고 위치   : (X=%d, Y=%d)\n" COLOR_RESET, acc_x, acc_y);
                                    printf(COLOR_RED "  * 내 차량 위치: (X=%d, Y=%d) -> 전방 %d m 지점 사고 발생!\n" COLOR_RESET, cur_x, cur_y, dist);
                                    printf(COLOR_YELLOW COLOR_BOLD "  ⚠️  [회피 권고] 사고 지점이 전방에 있습니다. 차선(y축)을 200으로 변경하세요!\n" COLOR_RESET);

                                    if (g_car.y != 200) {
                                        printf(COLOR_CYAN COLOR_BOLD "  🚗 [차선 변경 실행] 차선을 회피 차선으로 변경합니다: Y=%d -> Y=200\n" COLOR_RESET, g_car.y);
                                        g_car.y = 200;
                                    }
                                    printf(COLOR_RED COLOR_BOLD "======================================================================" COLOR_RESET "\n\n");
                                } else {
                                    printf("\n" COLOR_YELLOW "======================================================================" COLOR_RESET "\n");
                                    printf(COLOR_YELLOW "ℹ️ [후방 사고 알림] 차량 %s 가 후방 (X=%d, Y=%d)에서 사고 발생 (내 위치: X=%d)\n",
                                           acc_id, acc_x, acc_y, cur_x);
                                    printf(COLOR_YELLOW "  - 주행 경로에 영향이 없으므로 현재 차선(Y=%d)을 유지합니다.\n", cur_y);
                                    printf(COLOR_YELLOW "======================================================================" COLOR_RESET "\n\n");
                                }
                                pthread_mutex_unlock(&g_car.lock);
                            }
                        }
                    }
                }
                line_len = 0;
            } else if (ch != '\r') {
                if (line_len < (int)sizeof(line_buf) - 1) {
                    line_buf[line_len++] = ch;
                }
            }
        }
    }
    return NULL;
}

// 1초 주기 BSM 송신 스레드
void *bsm_thread_func(void *arg) {
    (void)arg;
    char bsm_msg[BUFFER_SIZE];

    while (g_car.running) {
        sleep(1);
        if (!g_car.running) break;

        pthread_mutex_lock(&g_car.lock);
        // 사고가 아닐 때만 x축 방향으로 속도만큼 전진
        if (!g_car.is_accident) {
            g_car.x += g_car.speed;
        }

        int cur_x = g_car.x;
        int cur_y = g_car.y;
        int cur_speed = g_car.speed;
        int is_acc = g_car.is_accident;
        pthread_mutex_unlock(&g_car.lock);

        // BSM 메시지 생성: BSM|차량ID|x,y|speed\n
        snprintf(bsm_msg, sizeof(bsm_msg), "BSM|%s|%d,%d|%d\n",
                 g_car.car_id, cur_x, cur_y, cur_speed);

        if (send(g_car.sock_fd, bsm_msg, strlen(bsm_msg), 0) < 0) {
            perror("BSM send failed");
            break;
        }

        if (is_acc) {
            printf(COLOR_YELLOW "💥 [내 BSM 송신 (사고 정지)] 위치: (X=%-4d, Y=%-3d) | 속도: %2d m/s" COLOR_RESET "\n",
                   cur_x, cur_y, cur_speed);
        } else {
            printf(COLOR_GREEN "📡 [내 BSM 송신] 위치: (X=%-4d, Y=%-3d) | 속도: %2d m/s" COLOR_RESET "\n",
                   cur_x, cur_y, cur_speed);
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("사용법: %s <차량ID> [시작X] [시작Y] [속도]\n", argv[0]);
        printf("예시 1: %s CAR01          (기본값: (300, 100), 속도: 40 m/s)\n", argv[0]);
        printf("예시 2: %s CAR02          (기본값: (0, 100), 속도: 50 m/s)\n", argv[0]);
        printf("예시 3: %s CAR03 0 50 15  (임의 좌표 및 속도)\n", argv[0]);
        return 1;
    }

    memset(&g_car, 0, sizeof(g_car));
    strncpy(g_car.car_id, argv[1], sizeof(g_car.car_id) - 1);
    g_car.running = 1;
    g_car.is_accident = 0;
    pthread_mutex_init(&g_car.lock, NULL);

    // 기본 좌표 및 속도 세팅
    if (strcasecmp(g_car.car_id, "CAR01") == 0) {
        g_car.x = 300;
        g_car.y = 100;
        g_car.speed = 40;
    } else if (strcasecmp(g_car.car_id, "CAR02") == 0) {
        g_car.x = 0;
        g_car.y = 100;
        g_car.speed = 50;
    } else {
        g_car.x = 0;
        g_car.y = 100;
        g_car.speed = 30;
    }

    // 사용자 지정 좌표 및 속도가 제공된 경우 덮어쓰기
    if (argc >= 4) {
        g_car.x = atoi(argv[2]);
        g_car.y = atoi(argv[3]);
    }
    if (argc >= 5) {
        g_car.speed = atoi(argv[4]);
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    // 소켓 생성 및 RSU 서버 연결
    g_car.sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_car.sock_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, DEFAULT_IP, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton() failed");
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }

    printf(COLOR_CYAN "RSU 서버 (%s:%d)에 접속 시도 중..." COLOR_RESET "\n", DEFAULT_IP, DEFAULT_PORT);
    if (connect(g_car.sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, COLOR_RED "RSU 서버 접속 실패! 먼저 rsu_server를 실행해 주세요." COLOR_RESET "\n");
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }

    // 1. RSU에 등록 요청 (HELLO | 차량ID)
    char hello_msg[BUFFER_SIZE];
    snprintf(hello_msg, sizeof(hello_msg), "HELLO|%s\n", g_car.car_id);
    if (send(g_car.sock_fd, hello_msg, strlen(hello_msg), 0) < 0) {
        perror("HELLO send failed");
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }

    // 2. WELCOME 응답 수신 대기
    char resp_buf[BUFFER_SIZE];
    ssize_t n = recv(g_car.sock_fd, resp_buf, sizeof(resp_buf) - 1, 0);
    if (n <= 0) {
        fprintf(stderr, COLOR_RED "RSU로부터 응답을 받지 못했습니다." COLOR_RESET "\n");
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }
    resp_buf[n] = '\0';

    if (strstr(resp_buf, "WELCOME") == NULL) {
        fprintf(stderr, COLOR_RED "RSU 등록 거부됨: %s" COLOR_RESET "\n", resp_buf);
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }

    printf(COLOR_GREEN COLOR_BOLD "======================================================" COLOR_RESET "\n");
    printf(COLOR_GREEN COLOR_BOLD "   [OBU] %s 등록 완료! (WELCOME 수신)               " COLOR_RESET "\n", g_car.car_id);
    printf(COLOR_GREEN "   - 시작 위치: (%d, %d), 속도: %d m/s\n", g_car.x, g_car.y, g_car.speed);
    printf(COLOR_YELLOW "   - [단축키 안내]\n");
    printf(COLOR_YELLOW "     'a' 또는 'A' : 현재 위치에서 사고 알림 발생 및 RSU 신고\n");
    printf(COLOR_YELLOW "     'q' 또는 'Q' : 프로그램 종료\n");
    printf(COLOR_GREEN COLOR_BOLD "======================================================" COLOR_RESET "\n\n");

    // 스레드 시작
    pthread_t recv_tid, bsm_tid;
    if (pthread_create(&recv_tid, NULL, recv_thread_func, NULL) != 0) {
        perror("pthread_create(recv) failed");
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }
    if (pthread_create(&bsm_tid, NULL, bsm_thread_func, NULL) != 0) {
        perror("pthread_create(bsm) failed");
        close(g_car.sock_fd);
        exit(EXIT_FAILURE);
    }

    // 터미널인 경우에만 raw 모드로 전환 (엔터 없이 즉시 키 입력 감지)
    if (isatty(STDIN_FILENO)) {
        set_raw_terminal_mode();
    }

    // 메인 스레드: 키보드 입력 감지 루프
    while (g_car.running) {
        int ch = getchar();
        if (ch == EOF) {
            if (!isatty(STDIN_FILENO)) {
                usleep(200000);
            }
            continue;
        }

        if (ch == 'a' || ch == 'A') {
            pthread_mutex_lock(&g_car.lock);
            if (!g_car.is_accident) {
                g_car.is_accident = 1;
                g_car.speed = 0; // 사고 후 정지
                int acc_x = g_car.x;
                int acc_y = g_car.y;
                pthread_mutex_unlock(&g_car.lock);

                // 사고 메시지 전송: ACCIDENT|차량ID|x,y\n
                char acc_msg[BUFFER_SIZE];
                snprintf(acc_msg, sizeof(acc_msg), "ACCIDENT|%s|%d,%d\n", g_car.car_id, acc_x, acc_y);
                send(g_car.sock_fd, acc_msg, strlen(acc_msg), 0);

                printf("\n" COLOR_RED COLOR_BOLD "💥💥💥 [사고 발생!] 내가 (%d, %d) 지점에서 사고를 냈습니다! RSU로 신고 완료! 💥💥💥" COLOR_RESET "\n\n",
                       acc_x, acc_y);
            } else {
                pthread_mutex_unlock(&g_car.lock);
                printf(COLOR_YELLOW "이미 사고 상태로 정지 중입니다." COLOR_RESET "\n");
            }
        } else if (ch == 'q' || ch == 'Q') {
            printf("\n" COLOR_YELLOW "[OBU] 사용자에 의해 종료됩니다." COLOR_RESET "\n");
            g_car.running = 0;
            break;
        }
    }

    reset_terminal_mode();
    close(g_car.sock_fd);
    pthread_cancel(recv_tid);
    pthread_cancel(bsm_tid);
    pthread_join(recv_tid, NULL);
    pthread_join(bsm_tid, NULL);
    pthread_mutex_destroy(&g_car.lock);

    return 0;
}
