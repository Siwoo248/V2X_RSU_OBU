#include "common.h"

typedef struct {
    int fd;
    char car_id[64];
    int registered;
    char recv_buf[BUFFER_SIZE];
    int buf_len;
} Client;

static Client clients[MAX_CLIENTS];

// 특정 클라이언트를 제외한 다른 모든 등록된 차량에게 메시지 전송
void broadcast_message(int sender_fd, const char *msg, int len) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd > 0 && clients[i].fd != sender_fd && clients[i].registered) {
            ssize_t sent = send(clients[i].fd, msg, len, 0);
            if (sent < 0) {
                perror("send() error in broadcast");
            }
        }
    }
}

// 클라이언트 연결 해제 및 리소스 정리
void remove_client(int idx) {
    if (clients[idx].fd > 0) {
        printf(COLOR_YELLOW "[RSU] 차량 '%s' 접속 종료 (소켓 %d)" COLOR_RESET "\n",
               clients[idx].registered ? clients[idx].car_id : "미등록", clients[idx].fd);
        close(clients[idx].fd);
        clients[idx].fd = -1;
        clients[idx].registered = 0;
        clients[idx].car_id[0] = '\0';
        clients[idx].buf_len = 0;
    }
}

// 개별 라인 메시지 파싱 및 처리
void handle_client_message(int idx, char *line) {
    // 줄 끝 개행문자 (\r, \n) 제거
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len--;
    }
    if (len == 0) return;

    // 파이프(|) 단위 파싱을 위해 복사본 생성
    char line_copy[BUFFER_SIZE];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';

    char *type = strtok(line_copy, "|");
    if (!type) return;

    // 1. HELLO | 차량ID -> 등록 요청
    if (strcmp(type, "HELLO") == 0) {
        char *id = strtok(NULL, "|");
        if (id) {
            // 앞뒤 공백 제거
            while (*id == ' ') id++;
            char *end = id + strlen(id) - 1;
            while (end > id && *end == ' ') *end-- = '\0';

            strncpy(clients[idx].car_id, id, sizeof(clients[idx].car_id) - 1);
            clients[idx].car_id[sizeof(clients[idx].car_id) - 1] = '\0';
            clients[idx].registered = 1;

            printf(COLOR_GREEN "[RSU 등록] 차량 등록 성공: %s (소켓: %d)" COLOR_RESET "\n",
                   clients[idx].car_id, clients[idx].fd);

            // WELCOME 응답 전송
            char welcome_msg[BUFFER_SIZE];
            snprintf(welcome_msg, sizeof(welcome_msg), "WELCOME|%s\n", clients[idx].car_id);
            send(clients[idx].fd, welcome_msg, strlen(welcome_msg), 0);
        }
    }
    // 2. BSM | 차량ID | 위치 | 속도 -> 상태 수신 (평상시 다른 차량에는 중계하지 않음)
    else if (strcmp(type, "BSM") == 0) {
        char *id = strtok(NULL, "|");
        char *pos = strtok(NULL, "|");
        char *speed = strtok(NULL, "|");

        if (id && pos && speed) {
            printf(COLOR_CYAN "[RSU 수신] BSM [%s] 위치: (%-10s), 속도: %2s m/s" COLOR_RESET "\n",
                   id, pos, speed);
            // 평상시 BSM은 다른 차량에 중계하지 않음 (사고 시에만 경고 전파)
        }
    }
    // 3. ACCIDENT | 차량ID | 위치 -> 사고 알림 수신 및 WARNING 브로드캐스트
    else if (strcmp(type, "ACCIDENT") == 0) {
        char *id = strtok(NULL, "|");
        char *pos = strtok(NULL, "|");

        if (id && pos) {
            printf(COLOR_RED COLOR_BOLD "🚨 [RSU 비상] 차량 [%s] (%s) 지점에서 사고 발생 접수! -> 전 차량 경고 브로드캐스트!" COLOR_RESET "\n",
                   id, pos);

            // WARNING 메시지 브로드캐스트
            char warning_msg[BUFFER_SIZE];
            snprintf(warning_msg, sizeof(warning_msg), "WARNING|%s|%s\n", id, pos);
            broadcast_message(clients[idx].fd, warning_msg, strlen(warning_msg));
        }
    }
    else {
        printf(COLOR_YELLOW "[RSU 경고] 알 수 없는 메시지 형식: %s" COLOR_RESET "\n", line);
    }
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int port = DEFAULT_PORT;
    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    // 클라이언트 목록 초기화
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].registered = 0;
        clients[i].car_id[0] = '\0';
        clients[i].buf_len = 0;
    }

    // 서버 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen() failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf(COLOR_GREEN COLOR_BOLD "======================================================" COLOR_RESET "\n");
    printf(COLOR_GREEN COLOR_BOLD "   RSU 서버가 시작되었습니다. (포트: %d)            " COLOR_RESET "\n", port);
    printf(COLOR_GREEN COLOR_BOLD "   차량(OBU)의 접속 및 BSM 중계를 대기합니다.         " COLOR_RESET "\n");
    printf(COLOR_GREEN COLOR_BOLD "======================================================" COLOR_RESET "\n\n");

    fd_set read_fds;
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd > 0) {
                FD_SET(clients[i].fd, &read_fds);
                if (clients[i].fd > max_fd) {
                    max_fd = clients[i].fd;
                }
            }
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            perror("select() error");
            break;
        }

        // 1. 신규 클라이언트 접속 요청 확인
        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int new_sock = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
            if (new_sock >= 0) {
                int added = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd < 0) {
                        clients[i].fd = new_sock;
                        clients[i].registered = 0;
                        clients[i].car_id[0] = '\0';
                        clients[i].buf_len = 0;
                        added = 1;
                        char client_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                        printf(COLOR_BLUE "[RSU] 새 클라이언트 연결 수락: IP %s, 소켓 %d" COLOR_RESET "\n",
                               client_ip, new_sock);
                        break;
                    }
                }
                if (!added) {
                    printf(COLOR_RED "[RSU] 최대 동시 접속 초과로 연결 거부 (소켓 %d)" COLOR_RESET "\n", new_sock);
                    close(new_sock);
                }
            }
        }

        // 2. 기존 클라이언트들의 데이터 수신 처리
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd > 0 && FD_ISSET(clients[i].fd, &read_fds)) {
                int space_left = sizeof(clients[i].recv_buf) - clients[i].buf_len - 1;
                if (space_left <= 0) {
                    // 버퍼 오버플로우 방지 초기화
                    clients[i].buf_len = 0;
                    space_left = sizeof(clients[i].recv_buf) - 1;
                }

                ssize_t bytes_read = recv(clients[i].fd, clients[i].recv_buf + clients[i].buf_len, space_left, 0);
                if (bytes_read <= 0) {
                    // 연결 종료 또는 에러
                    remove_client(i);
                } else {
                    clients[i].buf_len += bytes_read;
                    clients[i].recv_buf[clients[i].buf_len] = '\0';

                    // 줄바꿈(\n) 단위로 메시지 파싱
                    char *newline_pos;
                    while ((newline_pos = strchr(clients[i].recv_buf, '\n')) != NULL) {
                        *newline_pos = '\0';
                        char single_line[BUFFER_SIZE];
                        strncpy(single_line, clients[i].recv_buf, sizeof(single_line) - 1);
                        single_line[sizeof(single_line) - 1] = '\0';

                        // 다음 메시지 버퍼 앞으로 당기기
                        int line_len = strlen(single_line) + 1; // +1 for '\n'
                        int remaining = clients[i].buf_len - line_len;
                        if (remaining > 0) {
                            memmove(clients[i].recv_buf, newline_pos + 1, remaining);
                            clients[i].buf_len = remaining;
                            clients[i].recv_buf[clients[i].buf_len] = '\0';
                        } else {
                            clients[i].buf_len = 0;
                            clients[i].recv_buf[0] = '\0';
                        }

                        // 메시지 처리
                        handle_client_message(i, single_line);
                    }
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
