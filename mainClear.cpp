#include <arpa/inet.h> // ф-ции дл работы с IP-адресами(htons()..); дл преобразования сетевых адресов 
#include <netinet/in.h> // для sockaddr_in, AF_INET(IPv4)
#include <sys/socket.h> // для socket(), bind() ...
#include <unistd.h> // для POSIX ф-ций read(), write(), close()
#include <poll.h> // для poll(), pollfd
#include <signal.h> // для обработки сигнала

#include <iostream> // вывод в консоль
#include <string> // буфер для последнего сообщения 
#include <atomic> // потокобезопасный флаг для SIGINT 
#include <set> // множество IP:port(искл. дублирование адресов)
#include <mutex> // std::mutex - lock_guard
#include <thread> 
#include <vector>

// Потокобезопасное хранилище последнего сообщения и адресов 
struct address_store {
    // сохранить последнее сообщение 
    void add_message(const std::string& msg) {
        std::lock_guard<std::mutex> lock(mtx); // захват мьютекса 
        last_message = msg; // записать строку
    }

    // добавить новый адрес 
    void add_address(uint32_t ip, uint16_t port) {
        std::lock_guard<std::mutex> lock(mtx);
        addresses.emplace(ip, port); // пара IP:port(искл. дубли)
    }

    // получаем копию последнего сообщения 
    std::string get_last_message() {
        std::lock_guard<std::mutex> lock(mtx); // защита чтения 
        return last_message; // вернуть строку по значению
    }

    // Получаем список адресов
    std::vector<std::pair<uint32_t, uint16_t>> get_addresses() {
        std::lock_guard<std::mutex> lock(mtx);
        return std::vector<std::pair<uint32_t, uint16_t>>(addresses.begin(), addresses.end());
    };

    private:
        std::mutex mtx; // для синхронизации 
        std::string last_message; // последнее принятое сообщение 
        std::set<std::pair<uint32_t, uint16_t>> addresses; // уникальные адреса клиентов 
};

std::atomic<bool> running {true};

int sock_fd = -1; // UDP socket
int stop_pipe_fd[2] = {-1, -1}; // pipe для остановки r[0] w[1]
int msg_pipe_fd[2] = {-1, -1}; // // pipe для уведомления о новом собщении 
address_store store; // общее хранилище данных

//запись 1 байта в pipe 
void notify_pipe(int fd) {
    uint8_t b = 1; // факт записи
    write(fd, &b, sizeof(b)); // будим ожидающий поток
}

// обработчик сигнала 
void sigint_handler(int) {
    running = false;
    if (stop_pipe_fd[1] != 1)
    notify_pipe(stop_pipe_fd[1]); // будим потоки через stop_pipe
}

// поток приема 
void receiver() {
    char buf[1024]; // буфер для UDP пакета 
    sockaddr_in sender{}; // адрес отправителя 
    socklen_t len = sizeof(sender); // размер структуры адреса 

    // массив дескрипторов для poll() 
    pollfd fds[2] = {
        {sock_fd, POLLIN, 0}, // ожидаем данные на сокете 
        {stop_pipe_fd[0], POLLIN, 0} // ожидаем сигнал    
    };

    while (running.load()) {
        // блокируемся до события 
        int rc = poll(fds, 2, -1);
        if (rc < 0) {
            if (!running.load())
            break;

            continue;
        }

        // пришел stop_pipe
        if (fds[1].revents & POLLIN) {
            uint8_t b; // байт для чтения из pipe
            read(stop_pipe_fd[0], &b, sizeof(b)); // очищаем событие 
            break;
        }

        // если готов UDP сокет
        if (fds[0].revents & POLLIN) {
            // читаем датаграмму и адрес
            size_t n = recvfrom(sock_fd, buf, sizeof(buf) - 1, 0, (sockaddr*)&sender, &len);
            // если ошибка, пропускаем итерацию  
            if (n <= 0)
            continue;
            buf[n] = '\0';
            uint32_t ip = ntohl(sender.sin_addr.s_addr); // перевод IP из сетевого порядка в хостовый 
            uint16_t port = ntohs(sender.sin_port); // перевод port из сетевого порядка в хостовый

            store.add_message(buf); // сохранить ласт мэссэдж
            store.add_address(ip, port); // сохранить адрес 
            notify_pipe(msg_pipe_fd[1]); // уведомить поток отправки
            std::cout << "Получено >> " << buf << std::endl;
        }
    }
}

// поток отправки
void sender() {
    // массив дескрипторов для poll() 
    pollfd fds[2] = {
        {msg_pipe_fd[0], POLLIN, 0}, // ожидаем новове сообщение
        {stop_pipe_fd[0], POLLIN, 0} // ожидаем остановку
    };

    while (running.load()) {
        int rc  = poll(fds, 2, 3000);
        // если ошибка 
        if (rc < 0) {
            if (!running.load()) // если остановка
            break;
            
            continue;
        }

        // пришла остановка (stop_pipe)
        if (fds[1].revents & POLLIN) {
            uint8_t b;
            read(stop_pipe_fd[0], &b, sizeof(b)); // очистить pipe
            break;
        }

        //  пришло уведомление о новом сообщении
        if (fds[0].revents & POLLIN) {
            uint8_t b;
            read(msg_pipe_fd[0], &b, sizeof(b));
        }

        // если истек таймер или пришло новое мэссэндж
        if (rc == 0 || (fds[0].revents & POLLIN)) {
            // получаем последнее сообщение 
            std::string msg = store.get_last_message();

            // получаем список всех адресов 
            auto addresses = store.get_addresses();
            // если сообщение не пустое 
            if (!msg.empty()) {
                for (const auto& a : addresses) {
                    sockaddr_in peer{}; // стр-ра адреса получателя 
                    peer.sin_family = AF_INET;
                    peer.sin_addr.s_addr = htonl(a.first); // преобразуем IP в сетевой порядок
                    peer.sin_port = htons(a.second); // преобразуем порт в сетевой порядок

                    // отправка датаграммы 
                    sendto(sock_fd, msg.c_str(), msg.size(), 0, (sockaddr*)&peer, sizeof(peer));
                }
                std::cout << "Отправлено " << addresses.size() << " клиентам: " << msg << std::endl;
            }
        }
    }
}

int main() {
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0); // создаем сокет 
    // если ошибка 
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt =1; // значение для SO_REUSEADDR
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // повторное использование сокета 

    sockaddr_in addr{}; // адрес и инициализация сервера 
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    addr.sin_addr.s_addr = INADDR_ANY;

    // привязка сокета к порту, в случае ошибки выводим причину
    if (bind(sock_fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock_fd);
        return 1;
    }

    // создаем 2 pipe
    if (pipe(stop_pipe_fd) < 0 || pipe(msg_pipe_fd) < 0) {
        perror("pipe");

        // закрываем для read()
        if (stop_pipe_fd[0] != -1)
        close(stop_pipe_fd[0]);

        // закрываем для write()
        if (stop_pipe_fd[1] != -1)
        close(stop_pipe_fd[1]);

        // закрываем для read()
        if (msg_pipe_fd[0] != -1)
        close(msg_pipe_fd[0]);

        // закрываем для write()
        if (msg_pipe_fd[1] != -1)
        close(msg_pipe_fd[1]);

        close(sock_fd);
        return 1;
    }

    // установка обработчика для SIGINT
    signal(SIGINT, sigint_handler); 

    std::thread rx(receiver); // поток приема 
    std::thread sx(sender); // поток отправки 

    std::cout << "UDP сервер: 5000" << std::endl;

    rx.join(); // ожидаем поток приема
    sx.join(); // ожидаем поток отправки

    close(msg_pipe_fd[0]);
    close(msg_pipe_fd[1]);
    close(stop_pipe_fd[0]);
    close(stop_pipe_fd[1]);
    close(sock_fd);
    
    return 0;
}
