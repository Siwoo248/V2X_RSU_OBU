#include "common.h"
#include <termios.h>
#include <signal.h>

typedef struct {
    char car_id[64];
    int x;
    int y;
    int speed;
    int orig_speed;
    int is_accident;
    int running;
    int sock_fd;
    pthread_mutex_t lock;
    int alert_level;
    char status_msg[128];
} VehicleState;

static VehicleState g_car;
static struct termios orig_termios;
static int termios_modified = 0;
static RGBColor g_framebuffer[PIXEL_HEIGHT][PIXEL_WIDTH];

// 다른 차량 상태 (뒤차/추월차 CAR02)
static int g_other_x = -1;
static int g_other_y = -1;
static int g_other_speed = 0;
static int g_other_is_accident = 0;
static int g_other_active = 0;

void reset_terminal_mode(void) {
    if (termios_modified) {
        printf("\033[?25h\033[0m");
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        termios_modified = 0;
    }
}

void set_raw_terminal_mode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return;
    termios_modified = 1;
    atexit(reset_terminal_mode);

    struct termios new_termios = orig_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
    printf("\033[?25l");
}

void sigint_handler(int sig) {
    (void)sig;
    reset_terminal_mode();
    printf("\n" COLOR_YELLOW "[CAR01] 시스템을 종료합니다." COLOR_RESET "\n");
    exit(0);
}

// RSU 수신 스레드
void *recv_thread_func(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];
    char line_buf[BUFFER_SIZE];
    int line_len = 0;

    while (g_car.running) {
        ssize_t bytes = recv(g_car.sock_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) break;

        for (ssize_t i = 0; i < bytes; i++) {
            char ch = buffer[i];
            if (ch == '\n') {
                line_buf[line_len] = '\0';
                if (line_len > 0) {
                    char copy[BUFFER_SIZE];
                    strncpy(copy, line_buf, sizeof(copy) - 1);
                    copy[sizeof(copy) - 1] = '\0';

                    char *type = strtok(copy, "|");
                    if (type) {
                        if (strcmp(type, "BSM") == 0) {
                            char *id = strtok(NULL, "|");
                            char *pos = strtok(NULL, "|");
                            char *spd = strtok(NULL, "|");
                            if (id && pos && spd && strcmp(id, g_car.car_id) != 0) {
                                sscanf(pos, "%d,%d", &g_other_x, &g_other_y);
                                g_other_speed = atoi(spd);
                                g_other_active = 1;
                            }
                        } else if (strcmp(type, "WARNING") == 0) {
                            char *acc_id = strtok(NULL, "|");
                            char *acc_pos = strtok(NULL, "|");
                            if (acc_id && acc_pos) {
                                int ax = 0, ay = 0;
                                sscanf(acc_pos, "%d,%d", &ax, &ay);
                                g_other_x = ax;
                                g_other_y = ay;
                                g_other_is_accident = 1;
                                g_other_speed = 0;
                                g_other_active = 1;

                                // 만약 CAR02가 나보다 앞서가다가 사고 났다면 CAR01도 회피!
                                pthread_mutex_lock(&g_car.lock);
                                int dist = ax - g_car.x;
                                if (dist > 0 && dist <= SAFE_DISTANCE && g_car.y == ay) {
                                    g_car.y = (g_car.y == LANE_1_Y) ? LANE_2_Y : LANE_1_Y;
                                    g_car.speed = 35;
                                    g_car.alert_level = 2;
                                    snprintf(g_car.status_msg, sizeof(g_car.status_msg),
                                             "🚨 전방 %dm %s 사고 근접! ➜ 회피 차선 변경 실행", dist, acc_id);
                                }
                                pthread_mutex_unlock(&g_car.lock);
                            }
                        }
                    }
                }
                line_len = 0;
            } else if (ch != '\r' && line_len < (int)sizeof(line_buf) - 1) {
                line_buf[line_len++] = ch;
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
        if (!g_car.is_accident) {
            g_car.x += g_car.speed;
        }
        int cur_x = g_car.x;
        int cur_y = g_car.y;
        int cur_speed = g_car.speed;
        pthread_mutex_unlock(&g_car.lock);

        snprintf(bsm_msg, sizeof(bsm_msg), "BSM|%s|%d,%d|%d\n",
                 g_car.car_id, cur_x, cur_y, cur_speed);
        send(g_car.sock_fd, bsm_msg, strlen(bsm_msg), 0);
    }
    return NULL;
}

void init_road_background(void) {
    RGBColor c_guardrail = make_rgb(90, 100, 115);
    RGBColor c_asphalt   = make_rgb(38, 44, 54);
    RGBColor c_yellow    = make_rgb(250, 204, 21);
    RGBColor c_white     = make_rgb(220, 225, 230);

    for (int y = 0; y < PIXEL_HEIGHT; y++) {
        for (int x = 0; x < PIXEL_WIDTH; x++) {
            if (y == 0 || y == PIXEL_HEIGHT - 1) {
                g_framebuffer[y][x] = c_guardrail;
            } else if (y == 1 || y == PIXEL_HEIGHT - 2) {
                g_framebuffer[y][x] = c_white;
            } else if (y == 5 || y == 6) {
                if ((x % 10) < 6) g_framebuffer[y][x] = c_yellow;
                else g_framebuffer[y][x] = c_asphalt;
            } else {
                g_framebuffer[y][x] = c_asphalt;
            }
        }
    }
}

void draw_pixel_car(int px, int py, RGBColor body_color, int is_accident) {
    RGBColor c_tire   = make_rgb(15, 15, 15);
    RGBColor c_window = make_rgb(140, 210, 255);
    RGBColor c_light  = make_rgb(255, 255, 150);
    RGBColor c_fire1  = make_rgb(239, 68, 68);
    RGBColor c_fire2  = make_rgb(245, 158, 11);

    if (is_accident) {
        for (int dy = -1; dy <= 3; dy++) {
            for (int dx = -2; dx <= 9; dx++) {
                int sx = px + dx;
                int sy = py + dy;
                if (sx >= 0 && sx < PIXEL_WIDTH && sy >= 1 && sy < PIXEL_HEIGHT - 1) {
                    if ((dx + dy) % 2 == 0) g_framebuffer[sy][sx] = c_fire1;
                    else if ((dx * dy) % 3 == 0) g_framebuffer[sy][sx] = c_fire2;
                }
            }
        }
    }

    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 8; dx++) {
            int sx = px + dx;
            int sy = py + dy;
            if (sx < 0 || sx >= PIXEL_WIDTH || sy < 0 || sy >= PIXEL_HEIGHT) continue;

            RGBColor col = body_color;
            if ((dy == 0 || dy == 2) && (dx == 1 || dx == 6)) col = c_tire;
            else if ((dy == 0 || dy == 1) && (dx == 2 || dx == 3)) col = c_window;
            else if (dy == 1 && dx == 7 && !is_accident) col = c_light;

            g_framebuffer[sy][sx] = col;
        }
    }

    if (!is_accident) {
        for (int dx = 8; dx < 11; dx++) {
            int sx = px + dx;
            int sy = py + 1;
            if (sx < PIXEL_WIDTH) g_framebuffer[sy][sx] = make_rgb(180, 180, 110);
        }
    }
}

void *render_thread_func(void *arg) {
    (void)arg;

    while (g_car.running) {
        usleep(200000);
        if (!g_car.running) break;

        pthread_mutex_lock(&g_car.lock);
        int cur_x = g_car.x;
        int cur_y = g_car.y;
        int cur_spd = g_car.speed;
        int is_acc = g_car.is_accident;
        char status[128];
        strncpy(status, g_car.status_msg, sizeof(status) - 1);
        status[sizeof(status) - 1] = '\0';

        init_road_background();

        // 1. 내 차 (CAR01) 그리기: 28번째 픽셀에 고정 배치 (전후방 균형 뷰)
        int my_px = 28;
        int my_py = (cur_y == LANE_2_Y) ? 2 : 8;
        RGBColor my_color = is_acc ? make_rgb(239, 68, 68) : make_rgb(16, 185, 129); // 에메랄드 녹색
        draw_pixel_car(my_px, my_py, my_color, is_acc);

        // 2. 다른 차 (CAR02) 그리기: 1픽셀 = 6m 스케일
        if (g_other_active) {
            int dx = g_other_x - cur_x;
            int other_px = my_px + (dx / 6);
            int other_py = (g_other_y == LANE_2_Y) ? 2 : 8;
            if (other_px >= -8 && other_px < PIXEL_WIDTH) {
                RGBColor o_color = g_other_is_accident ? make_rgb(239, 68, 68) : make_rgb(2, 132, 199);
                draw_pixel_car(other_px, other_py, o_color, g_other_is_accident);
            }
        }
        pthread_mutex_unlock(&g_car.lock);

        printf("\033[H\033[J");

        printf(COLOR_CYAN COLOR_BOLD "======================= [ OBU 1 : CAR01 (앞차 단말기) ] =======================" COLOR_RESET "\n");
        printf(COLOR_BOLD " [🛣️  실시간 도로 24-bit TrueColor 그래픽]" COLOR_RESET "\n");

        for (int y = 0; y < PIXEL_HEIGHT; y += 2) {
            if (y == 2) printf(COLOR_CYAN " 2차로 " COLOR_RESET);
            else if (y == 8) printf(COLOR_WHITE " 1차로 " COLOR_RESET);
            else printf("       ");

            for (int x = 0; x < PIXEL_WIDTH; x++) {
                RGBColor top = g_framebuffer[y][x];
                RGBColor bot = g_framebuffer[y + 1][x];
                printf("\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm▀", top.r, top.g, top.b, bot.r, bot.g, bot.b);
            }
            printf("\033[0m\n");
        }
        printf("       " COLOR_GRAY "├───────────────┼───────────────┼───────────────┼───────────────┤" COLOR_RESET "\n");
        printf("       " COLOR_GRAY "-140m           -70m           내위치          +70m           +140m" COLOR_RESET "\n");
        printf("-----------------------------------------------------------------------------\n");

        int lane_num = (cur_y == LANE_2_Y) ? 2 : 1;
        printf(" 📍 " COLOR_BOLD "내 차량 위치" COLOR_RESET ": X = %-4dm, Y = %-3d (%d차로)  |  🚀 " COLOR_BOLD "속도" COLOR_RESET ": %2d m/s\n",
               cur_x, cur_y, lane_num, cur_spd);

        if (is_acc) {
            printf(" 💥 " COLOR_RED COLOR_BOLD "상태: [사고 발생!] (%d, %d) 지점에서 충돌 정지됨! RSU로 긴급 신고 완료" COLOR_RESET "\n",
                   cur_x, cur_y);
        } else {
            printf(" 💡 " COLOR_GREEN "상태: %s" COLOR_RESET "\n", status);
        }

        printf(" ⌨️  " COLOR_YELLOW "단축키: 'a' 사고 발생 및 RSU 긴급 신고  |  'q' 프로그램 종료" COLOR_RESET "\n");
        printf(COLOR_CYAN "=============================================================================" COLOR_RESET "\n");
        fflush(stdout);
    }
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    memset(&g_car, 0, sizeof(g_car));
    strncpy(g_car.car_id, "CAR01", sizeof(g_car.car_id) - 1);
    g_car.x = 400; // 앞차 시작 위치: 400m
    g_car.y = LANE_1_Y;
    g_car.speed = 60; // 앞차 속도: 60 m/s
    g_car.orig_speed = 60;
    g_car.running = 1;
    g_car.is_accident = 0;
    g_car.alert_level = 0;
    snprintf(g_car.status_msg, sizeof(g_car.status_msg), "정상 주행 중 (1차로 주행)");
    pthread_mutex_init(&g_car.lock, NULL);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    g_car.sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_car.sock_fd < 0) {
        perror("socket error");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DEFAULT_PORT);
    inet_pton(AF_INET, DEFAULT_IP, &serv_addr.sin_addr);

    printf("RSU 서버 (%s:%d)에 접속 시도 중...\n", DEFAULT_IP, DEFAULT_PORT);
    if (connect(g_car.sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "RSU 서버 접속 실패! rsu_server를 먼저 실행하세요.\n");
        close(g_car.sock_fd);
        return 1;
    }

    send(g_car.sock_fd, "HELLO|CAR01\n", 12, 0);
    char buf[128];
    recv(g_car.sock_fd, buf, sizeof(buf), 0);

    if (isatty(STDIN_FILENO)) {
        set_raw_terminal_mode();
    }

    pthread_t recv_tid, bsm_tid, render_tid;
    pthread_create(&recv_tid, NULL, recv_thread_func, NULL);
    pthread_create(&bsm_tid, NULL, bsm_thread_func, NULL);
    pthread_create(&render_tid, NULL, render_thread_func, NULL);

    while (g_car.running) {
        int ch = getchar();
        if (ch == EOF) {
            usleep(100000);
            continue;
        }

        if (ch == 'a' || ch == 'A') {
            pthread_mutex_lock(&g_car.lock);
            if (!g_car.is_accident) {
                g_car.is_accident = 1;
                g_car.speed = 0;
                int acc_x = g_car.x;
                int acc_y = g_car.y;
                snprintf(g_car.status_msg, sizeof(g_car.status_msg),
                         "💥 내가 (%d, %d) 지점에서 사고 발생! RSU로 긴급 신고 완료!", acc_x, acc_y);
                pthread_mutex_unlock(&g_car.lock);

                char acc_msg[BUFFER_SIZE];
                snprintf(acc_msg, sizeof(acc_msg), "ACCIDENT|CAR01|%d,%d\n", acc_x, acc_y);
                send(g_car.sock_fd, acc_msg, strlen(acc_msg), 0);
            } else {
                pthread_mutex_unlock(&g_car.lock);
            }
        } else if (ch == 'q' || ch == 'Q') {
            g_car.running = 0;
            break;
        }
    }

    reset_terminal_mode();
    close(g_car.sock_fd);
    pthread_cancel(recv_tid);
    pthread_cancel(bsm_tid);
    pthread_cancel(render_tid);
    pthread_join(recv_tid, NULL);
    pthread_join(bsm_tid, NULL);
    pthread_join(render_tid, NULL);
    pthread_mutex_destroy(&g_car.lock);

    printf("\n[CAR01] 프로그램이 종료되었습니다.\n");
    return 0;
}
