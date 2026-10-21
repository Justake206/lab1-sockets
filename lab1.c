#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>       // Для вывода таблицы FD (/proc/self/fd)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Глобальная переменная для обработчика сигналов (чтобы закрыть сокет)
static volatile sig_atomic_t g_sockfd = -1;

// 1. Обработчик сигнала SIGINT (Ctrl+C) для корректного закрытия
void sigint_handler(int sig) {
    (void)sig;
    if (g_sockfd >= 0) {
        close(g_sockfd);
    }
    write(STDERR_FILENO, "\n[SIGINT] Программа прервана. Сокет закрыт.\n", 48);
    _exit(EXIT_FAILURE);
}

// 2. Функция для вывода списка открытых файловых дескрипторов
void print_open_fds(const char *label) {
    printf("\n--- Открытые FD (%s) ---\n", label);
    DIR *dir = opendir("/proc/self/fd");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] != '.') {
                printf("FD: %s\n", entry->d_name);
            }
        }
        closedir(dir);
    } else {
        perror("opendir /proc/self/fd");
    }
    printf("------------------------\n");
}

// 3. Функция автоматического закрытия сокета при ошибке
// Сюда мы прыгаем через goto при любой ошибке
void cleanup_and_exit(int sockfd, int exit_code) {
    if (sockfd >= 0) {
        printf("Закрытие сокета (cleanup)...\n");
        close(sockfd);
    }
    exit(exit_code);
}

int main(int argc, char *argv[]) {
    // Проверка аргументов командной строки
    if (argc < 5) {
        fprintf(stderr, "Использование: %s <domain> <type> <ip> <port> [--bind]\n", argv[0]);
        fprintf(stderr, "Пример: %s AF_INET SOCK_STREAM 127.0.0.1 8080 --bind\n", argv[0]);
        return EXIT_FAILURE;
    }

    // --- ШАГ А. ПАРСИНГ АРГУМЕНТОВ ---
    int domain, type, port;
    const char *ip_str = argv[3];
    int do_bind = (argc >= 6 && strcmp(argv[5], "--bind") == 0);

    // Domain
    if (strcmp(argv[1], "AF_INET") == 0) domain = AF_INET;
    else if (strcmp(argv[1], "AF_INET6") == 0) domain = AF_INET6;
    else {
        fprintf(stderr, "Ошибка: Неверный domain '%s'. Используйте AF_INET или AF_INET6\n", argv[1]);
        return EXIT_FAILURE;
    }

    // Type
    if (strcmp(argv[2], "SOCK_STREAM") == 0) type = SOCK_STREAM;
    else if (strcmp(argv[2], "SOCK_DGRAM") == 0) type = SOCK_DGRAM;
    else {
        fprintf(stderr, "Ошибка: Неверный type '%s'. Используйте SOCK_STREAM или SOCK_DGRAM\n", argv[2]);
        return EXIT_FAILURE;
    }

    // Port
    char *endptr;
    long p = strtol(argv[4], &endptr, 10);
    if (*endptr != '\0' || p < 0 || p > 65535) {
        fprintf(stderr, "Ошибка: Неверный порт '%s'. Допустимо от 0 до 65535\n", argv[4]);
        return EXIT_FAILURE;
    }
    port = (int)p;

    // --- ШАГ Б. ВАЛИДАЦИЯ IP ---
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    void *addr_ptr;
    
    memset(&addr4, 0, sizeof(addr4));
    memset(&addr6, 0, sizeof(addr6));

    if (domain == AF_INET) {
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(port);
        if (inet_pton(AF_INET, ip_str, &addr4.sin_addr) <= 0) {
            fprintf(stderr, "Ошибка: Неверный IPv4 адрес '%s'\n", ip_str);
            return EXIT_FAILURE;
        }
        addr_ptr = &addr4;
    } else {
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);
        if (inet_pton(AF_INET6, ip_str, &addr6.sin6_addr) <= 0) {
            fprintf(stderr, "Ошибка: Неверный IPv6 адрес '%s'\n", ip_str);
            return EXIT_FAILURE;
        }
        addr_ptr = &addr6;
    }

    // Демонстрация разницы inet_pton vs inet_addr для IPv6
    printf("\n[Инфо] Проверка IPv4-функцией inet_addr(): %s\n", ip_str);
    if (domain == AF_INET6) {
        printf("[Инфо] inet_addr() не поддерживает IPv6 и вернет ошибку (INADDR_NONE).\n");
    }

    // --- ШАГ В. ПОДГОТОВКА И СИГНАЛЫ ---
    signal(SIGINT, sigint_handler);
    print_open_fds("до создания сокета");

    // --- ШАГ Г. СОЗДАНИЕ СОКЕТА ---
    int sockfd = socket(domain, type, 0);
    if (sockfd < 0) {
        perror("Ошибка socket()");
        return EXIT_FAILURE; // Закрывать нечего, сокет не создан
    }

    printf("\nСокет успешно создан! FD: %d\n", sockfd);
    if (sockfd < 3) {
        fprintf(stderr, "Предупреждение: FD меньше 3 (странно, но бывает).\n");
    }
    
    // Сохраняем в глобальную переменную для обработчика SIGINT
    g_sockfd = sockfd;
    print_open_fds("после создания сокета");

    // --- ШАГ Д. ВЫВОД ТИПА СОКЕТА (Проверка ядра) ---
    int sock_type;
    socklen_t optlen = sizeof(sock_type);
    if (getsockopt(sockfd, SOL_SOCKET, SO_TYPE, &sock_type, &optlen) == 0) {
        printf("Проверка ядра -> Тип сокета: %s\n", 
               sock_type == SOCK_STREAM ? "TCP (SOCK_STREAM)" : "UDP (SOCK_DGRAM)");
    }

    // --- ШАГ Е. BIND ---
    if (do_bind) {
        printf("\nПопытка bind()...\n");
        if (bind(sockfd, (struct sockaddr *)addr_ptr, domain == AF_INET ? sizeof(addr4) : sizeof(addr6)) < 0) {
            perror("Ошибка bind()"); 
            // Вместо простого return используем нашу очистку:
            goto error_cleanup; 
        }
        printf("Успешно привязано к %s:%d\n", ip_str, port);

        // Если порт был 0, узнаем, какой выбрало ядро
        if (port == 0) {
            struct sockaddr_storage actual_addr;
            socklen_t len = sizeof(actual_addr);
            if (getsockname(sockfd, (struct sockaddr *)&actual_addr, &len) == 0) {
                if (domain == AF_INET) {
                    printf("Ядро выбрало порт: %d\n", ntohs(((struct sockaddr_in*)&actual_addr)->sin_port));
                } else {
                    printf("Ядро выбрало порт: %d\n", ntohs(((struct sockaddr_in6*)&actual_addr)->sin6_port));
                }
            }
        }
    } else {
        printf("\nФлаг --bind не указан. Привязка пропущена.\n");
    }

    // --- ШАГ Ж. ЗАВЕРШЕНИЕ (УСПЕШНОЕ) ---
    printf("\nРабота завершена. Закрываем сокет...\n");
    if (close(sockfd) < 0) {
        perror("Ошибка close()");
        return EXIT_FAILURE;
    }
    printf("Сокет успешно закрыт.\n");
    g_sockfd = -1; // Обнуляем, чтобы SIGINT не пытался закрыть его снова

    print_open_fds("после закрытия сокета");
    return EXIT_SUCCESS;

    // --- ОБРАБОТКА ОШИБОК (goto) ---
error_cleanup:
    printf("\n[ОШИБКА] Произошла ошибка. Автоматическое закрытие сокета...\n");
    cleanup_and_exit(sockfd, EXIT_FAILURE);
}