/*
    FTP指令: http://www.nsftools.com/tips/RawFTP.htm
    支持指令
    cd, list, pwd, mkdir, put, get, setlimit, size, port, pasv
    delete, rmdir, rename, ascii, binary, quit
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "log.h"

#define FTP_PORT_MODE 1
#define FTP_PASV_MODE 2
#define BUFF_SIZE 1024
static char FTP_SERVER_IP[BUFF_SIZE];
static char FTP_CLIENT_IP[BUFF_SIZE];
static int FTP_PORT;
static int FTP_DATA_MODE;  // FTP 主动/被动模式
static int FTP_DATA_PORT;  // FTP client数据传输端口 由port或者pasv端口打开
static int FTP_BYTES_PER_SEC;  // 流量控制, 每second多少byte

/* 工具函数 */
int gettoken(const char* src, char* res);
void fflushStdin();
int getPassword(char* password, int size);
const char* skipResponseCode(const char* response);

/* FTP 指令 */
int FTPPasv(int ftp_ctl_fd);
int FTPPort(int ftp_ctl_fd, const char* port_cmd);

int FTPRest(int ftp_ctl_fd, long int offset);
int FTPStor(int ftp_ctl_fd, const char* filename);
int FTPAppe(int ftp_ctl_fd, const char* filename);
int FTPRetr(int ftp_ctl_fd, const char* filename);
int FTPCd(int ftp_ctl_fd, char* path);
int FTPList(int ftp_ctl_fd);
int FTPPwd(int ftp_ctl_fd);
int FTPMkdir(int ftp_ctl_fd, const char* dirname);
long FTPSize(int ftp_ctl_fd, const char* filename);
int FTPDele(int ftp_ctl_fd, const char* filename);
int FTPRmd(int ftp_ctl_fd, const char* dirname);
int FTPRename(int ftp_ctl_fd, const char* oldfilename, const char* newfilename);
int FTPAscii(int ftp_ctl_fd);
int FTPBinary(int ftp_ctl_fd);
int FTPQuit(int ftp_ctl_fd);

/* FTP 操作 */
void FTPSetRateLimit(double ftp_rate_limit_kb);
void FTPCommand(int ftp_ctl_fd);
int FTPCheckResponse(const char* response);
int FTPTransmit(int dest_fd, int src_fd, void* trans_buf);
int FTPGet(int ftp_ctl_fd, const char* filename, const char* newfilename);
int FTPPut(int ftp_ctl_fd, const char* filename, const char* newfilename);
int FTPConnect(const char* addr, int port);
int FTPOpenDataSockfd(int ftp_ctl_fd);
int FTPParseCommand(int ftp_ctl_fd, const char* cmd);
int FTPLogin(int ftp_ctl_fd, const char* username, const char* password);

/* 数据缓冲区 */
static char recv_buf[BUFF_SIZE], send_buf[BUFF_SIZE];

/* ---------------------------------- */

int gettoken(const char* src, char* res) {
    while (*src == ' ') src++;
    int i = 0;
    while (*src != ' ' && *src != '\0') {
        res[i] = *src;
        src++;
        i++;
    }
    res[i] = '\0';
    return i;
}

void fflushStdin() {
    char ch;
    while ((ch = getchar()) != EOF && ch != '\n')
        ;
}

int getPassword(char* password, int size) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    int c, n = 0;
    do {
        c = getchar();
        if (c != '\n' && c != 'r' && c != 127) {  // 普通输入
            password[n] = c;
            printf("*");
            n++;
        } else if ((c != '\n' || c != '\r') && c == 127) {  // 判断退格
            if (n > 0) {
                n--;
                printf("\b \b");
            }
        }
    } while (c != '\n' && c != '\r' && n < (size - 1));
    password[n] = '\0';

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return n;
}

const char* skipResponseCode(const char* response) {
    while (*response != ' ') response++;
    return response + 1;
}

/* ---------------------------------- */

/*
    命令 "REST offset\r\n"
*/
int FTPRest(int ftp_ctl_fd, long int offset) {
    sprintf(send_buf, "REST %ld\r\n", offset);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< REST failed. %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "PASV\r\n"
    客户端发送命令改变FTP数据模式为被动模式
*/
int FTPPasv(int ftp_ctl_fd) {
    if (FTP_DATA_MODE == FTP_PORT_MODE) close(FTP_DATA_PORT);

    sprintf(send_buf, "PASV\r\n");
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< PASV failed. %s\n", recv_buf);
        return -1;
    }
    int port = -1, h1, h2, h3, h4, p1, p2;
    sscanf(recv_buf, "%*[^(](%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2);
    port = p1 * 256 + p2;

    FTP_DATA_MODE = FTP_PASV_MODE;
    FTP_DATA_PORT = port;

    return 0;
}

/*
    命令 "PORT h1,h2,h3,h4,p1,p2\r\n"
    客户端发送命令改变FTP数据模式为主动模式
*/
int FTPPort(int ftp_ctl_fd, const char* port_cmd) {
    if (FTP_DATA_MODE == FTP_PORT_MODE) close(FTP_DATA_PORT);

    int h1, h2, h3, h4, p1, p2;
    sscanf(port_cmd, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2);
    LOGI("PORT %d,%d,%d,%d,%d,%d\n", h1, h2, h3, h4, p1, p2);

    int port = p1 * 256 + p2;
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOGE("[socket]\n");
        return -1;
    }

    int opt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_fd, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        LOGE("[bind]\n");
        return -1;
    }

    if (listen(sock_fd, 64) == -1) {
        LOGE("[listen]\n");
        close(sock_fd);
        return -1;
    }

    sprintf(send_buf, "PORT %d,%d,%d,%d,%d,%d\r\n", h1, h2, h3, h4, p1, p2);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< PORT failed. %s\n", recv_buf);
        close(sock_fd);
        return -1;
    }

    FTP_DATA_MODE = FTP_PORT_MODE;
    FTP_DATA_PORT = sock_fd;

    return 0;
}

/*
    命令 "STOR filename\r\n"
    客户端发送命令上传文件至服务器端
*/
int FTPStor(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "STOR %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< STOR %s failed. %s", filename, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "APPE filename\r\n"
    上传添加到指定文件末尾
*/
int FTPAppe(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "APPE %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< APPE %s failed. %s", filename, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "RETR filename\r\n"
    客户端发送命令从服务器端下载文件
*/
int FTPRetr(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "RETR %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< RETR %s failed. %s", filename, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "CWD dirname\r\n"
    客户端发送命令改变工作目录
    客户端接收服务器的响应码和信息
*/
int FTPCd(int ftp_ctl_fd, char* dirname) {
    // 当dirname为空时, 设置为 "."
    if (strlen(dirname) == 0) {
        dirname[0] = '.';
        dirname[1] = '\0';
    }
    sprintf(send_buf, "CWD %s\r\n", dirname);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< CWD %s failed. %s", dirname, recv_buf);
        return -1;
    }
    // printf("cd %s ok.\n", dirname);
    return 0;
}

/*
    命令 "list dirname\r\n"
    客户端发送命令获取指定目录文件列表或者指定文件信息
    客户端接收服务器的响应码和信息
    正常为 "125 Data connection already open. Transfer starting."
    结束为 "226 Transfer complete."
*/
int FTPList(int ftp_ctl_fd) {
    // 打开数据传输套接字
    int ftp_data_fd = -1;
    // 被动模式
    if (FTP_DATA_MODE == FTP_PASV_MODE) {
        ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);
    }

    sprintf(send_buf, "LIST -al\r\n");
    FTPCommand(ftp_ctl_fd);
    // 125 Data connection already open. Transfer starting.
    if (FTPCheckResponse(recv_buf)) {
        printf("<< LIST failed. %s", recv_buf);
        close(ftp_data_fd);
        return -1;
    }

    // 主动模式
    if (FTP_DATA_MODE == FTP_PORT_MODE) {
        ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);
    }

    // read data
    int nread;
    for (;;) {
        /* data to read from socket */
        if ((nread = read(ftp_data_fd, recv_buf, BUFF_SIZE)) < 0)
            printf("<< recv error\n");
        else if (nread == 0)
            break;

        if (write(STDOUT_FILENO, recv_buf, nread) != nread)
            printf("<< send error to stdout\n");
    }

    // 关闭数据套接字
    close(ftp_data_fd);

    // 226 Transfer complete.
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< LIST failed. %s", recv_buf);
        return -1;
    }

    return 0;
}

/*
    命令 "pwd\r\n"
    客户端发送命令获取当前所在路径
    客户端接收服务器的响应码和信息
*/
int FTPPwd(int ftp_ctl_fd) {
    sprintf(send_buf, "PWD\r\n");
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< PWD failed. %s", recv_buf);
        return -1;
    }
    // 去除开头的response code
    printf("%s", skipResponseCode(recv_buf));
    // printf("PWD ok.\n");
    return 0;
}

/*
    命令 "MKD dirname\r\n"
    创建目录
*/
int FTPMkdir(int ftp_ctl_fd, const char* dirname) {
    sprintf(send_buf, "MKD %s\r\n", dirname);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< MKD %s failed. %s", dirname, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "SIZE filename\r\n"
    客户端发送命令从服务器端得到下载文件的大小
    客户端接收服务器的响应码和信息，正常为 "213 <size>"
*/
long FTPSize(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "SIZE %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< SIZE %s failed. %s", filename, recv_buf);
        return -1;
    }
    return atol(skipResponseCode(recv_buf));
}

/*
    命令 "DELE filename\r\n"
    创建目录
*/
int FTPDele(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "DELE %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< DELE %s failed. %s", filename, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "RMD dirname\r\n"
    创建目录
*/
int FTPRmd(int ftp_ctl_fd, const char* dirname) {
    sprintf(send_buf, "RMD %s\r\n", dirname);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< RMD %s failed. %s", dirname, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "RNFR oldfilename\r\n"
    响应 "350 Ready for destination name."
    命令 "RNTO newfilename\r\n"
    响应 "250 Renaming ok."
    重新命名filename
*/
int FTPRename(int ftp_ctl_fd,
              const char* oldfilename,
              const char* newfilename) {
    sprintf(send_buf, "RNFR %s\r\n", oldfilename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< RNFR %s failed. %s", oldfilename, recv_buf);
        return -1;
    }
    sprintf(send_buf, "RNTO %s\r\n", newfilename);
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< RNTO %s failed. %s", newfilename, recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "TYPE A\r\n"
    客户端与服务端将改变传输模式为ASCII模式
    客户端接收服务器的响应码
    正常为 "200 Type set to: Ascii."
*/
int FTPAscii(int ftp_ctl_fd) {
    sprintf(send_buf, "TYPE A\r\n");
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< TYPE A failed. %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "TYPE I\r\n"
    客户端与服务端将改变传输模式为二进制模式
    客户端接收服务器的响应码
    正常为 "200 Type set to: Binary."
*/
int FTPBinary(int ftp_ctl_fd) {
    sprintf(send_buf, "TYPE I\r\n");
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< TYPE I failed. %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "QUIT\r\n"
    客户端将断开与服务器端的连接
    客户端接收服务器的响应码
    正常为 "221 Goodbye."
*/
int FTPQuit(int ftp_ctl_fd) {
    sprintf(send_buf, "QUIT\r\n");
    FTPCommand(ftp_ctl_fd);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< QUIT failed. %s", recv_buf);
        return -1;
    }
    printf("%s", skipResponseCode(recv_buf));
    /* 客户端关闭控制连接 */
    close(ftp_ctl_fd);
    return 0;
}

/* ---------------------------------- */

/*
    命令 "setlimit %d"
    <=0 不限速 单位 KB/s
*/
void FTPSetRateLimit(double ftp_rate_limit_kb) {
    if (ftp_rate_limit_kb < 0) {
        FTP_BYTES_PER_SEC = -1;
    } else {
        FTP_BYTES_PER_SEC = (int) (ftp_rate_limit_kb * 1024);
    }
}

/*
    检查返回值
    判断命令是否正确执行
*/
int FTPCheckResponse(const char* response) {
    // printf("<< check: %s", response);
    if (response[0] == '2' || response[0] == '1') {
        // 202 Command not implemented, superfluous at this site.
        if (strncmp(response, "202", 3) == 0) return 1;
        return 0;
    } else {
        // 350 Requested file action pending further information U
        if (strncmp(response, "350", 3) == 0) return 0;
        return 1;
    }
}

/*
    src_fd 传输数据到 dest_fd
*/
int FTPTransmit(int dest_fd, int src_fd, void* trans_buf) {
    int flag = 0;  // 跳出循环标志
    int64_t limit_bytes = BUFF_SIZE, total_trans_bytes = 0;
    time_t cur_time = time(NULL), nx_time;
    size_t nleft;
    ssize_t nread;
    // 客户端通过数据连接 从服务器接收文件内容
    while (1) {
        if (FTP_BYTES_PER_SEC > 0) {
            nx_time = time(NULL);
            while (nx_time - cur_time < 1) {
                usleep(200000);  // 休眠200ms
                nx_time = time(NULL);
            }
            limit_bytes = (nx_time - cur_time) * FTP_BYTES_PER_SEC;
            cur_time = nx_time;
        }
        nleft = limit_bytes;
        while (nleft > 0) {
            nread = nleft > BUFF_SIZE ? BUFF_SIZE : nleft;
            nread = read(src_fd, trans_buf, nread);
            if (nread <= 0) {
                flag = 1;
                break;
            }
            /* 客户端写文件 */
            if (write(dest_fd, trans_buf, nread) < 0) {
                LOGE("write error.\n");
            }
            nleft -= nread;
        }
        total_trans_bytes += limit_bytes - nleft;
        // printf("<< Alreay transmitted %lld bytes\n", total_trans_bytes);
        if (flag) {
            break;
        }
    }
    return 0;
}

int FTPPut(int ftp_ctl_fd, const char* filename, const char* newfilename) {
    // 检查本地文件是否存在
    if (access(filename, F_OK) < 0) {
        printf("%s No such file or directory.\n", filename);
        return -1;
    }

    // 未给出上传后新的文件名则使用源文件名
    if (strlen(newfilename) == 0) {
        newfilename = filename;
    }

    // 打开传输fd
    int ftp_data_fd = -1;

    // 被动模式 每次传输都需要重新打开
    if (FTP_DATA_MODE == FTP_PASV_MODE) {
        ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);
    }

    /* 客户端打开文件并判断是否断点续传 */
    int file_handle = open(filename, O_RDONLY, 0);
    if (file_handle < 0) {
        LOGE("open error!\n");
        return -1;
    }
    long int ftp_file_size = -1;
    if ((ftp_file_size = FTPSize(ftp_ctl_fd, filename)) != -1) {
        // 存在文件 断点续传
        int err = 0;     // 错误标示
        int resume = 0;  // 恢复到覆盖上传模式
        long int offset = 0;
        if ((offset = lseek(file_handle, 0, SEEK_END)) != -1) {
            if (offset == ftp_file_size) {
                // 文件大小相同认为文件相同
                printf("File exists.\n");
                err = 1;
            } else if (offset < ftp_file_size) {
                // 不相同文件 需要覆盖
                // 传输上传文件指令 STOR
                resume = 1;

                if (!err && FTPStor(ftp_ctl_fd, newfilename) == -1) {
                    err = 1;
                }
            }
            // 定位本地文件offset到续传处
            if (!err && lseek(file_handle, ftp_file_size, SEEK_SET) == -1) {
                LOGE("lseek error.\n");
                err = 1;
            }
            // 如果断点续传失败 则取消下载
            if (!err && !resume && FTPAppe(ftp_ctl_fd, newfilename) == -1) {
                printf("APPE %s resume from break-point failed.\n",
                       newfilename);
                err = 1;
            }

        } else {
            err = 1;
            LOGE("lseek failed.\n");
        }

        if (err && !resume) {
            if (ftp_data_fd != -1) close(ftp_data_fd);
            close(file_handle);
            return -1;
        }
    } else {
        // 不存在文件
        // 传输上传文件指令 STOR
        if (FTPStor(ftp_ctl_fd, newfilename) == -1) {
            if (ftp_data_fd != -1) close(ftp_data_fd);
            close(file_handle);
            return -1;
        }
    }

    // 主动模式
    if (FTP_DATA_MODE == FTP_PORT_MODE) {
        ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);
    }

    FTPTransmit(ftp_data_fd, file_handle, send_buf);

    /* 关闭数据传输套接字 */
    close(ftp_data_fd);
    /* 客户端关闭文件 */
    close(file_handle);

    // 226 Transfer complete.
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< PUT %s failed. %s", newfilename, recv_buf);
        return -1;
    }

    // printf("put ok.\n");
    return 0;
}

int FTPGet(int ftp_ctl_fd, const char* filename, const char* newfilename) {
    if (FTPSize(ftp_ctl_fd, filename) == -1) {
        return -1;
    }
    int ftp_file_size = -1;
    sscanf(skipResponseCode(recv_buf), "%d", &ftp_file_size);

    // 下载文件是否重命名
    if (strlen(newfilename) == 0) {
        newfilename = filename;
    }

    int file_handle = -1;
    if (access(newfilename, F_OK) == 0) {
        // 如果文件存在 断点续传
        file_handle = open(newfilename, O_WRONLY | O_APPEND);
        if (file_handle < 0) {
            LOGE("open error!\n");
            return -1;
        }
        int err = 0;  // 错误标示
        long int offset = 0;
        if ((offset = lseek(file_handle, 0, SEEK_END)) != -1) {
            if (offset == ftp_file_size) {
                printf("File exists.\n");
                err = 1;
            }
            // 如果断点续传失败 则取消下载
            if (!err && FTPRest(ftp_ctl_fd, offset) == -1) {
                printf("STOR %s resume from break-point failed.\n",
                       newfilename);
                err = 1;
            }
        } else {
            err = 1;
            LOGE("lseek failed.\n");
        }
        if (err) {
            close(file_handle);
            return -1;
        }
    } else {
        file_handle = open(newfilename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (file_handle < 0) {
            LOGE("open error!\n");
            return -1;
        }
    }

    // 打开传输fd
    int ftp_data_fd = -1;

    // 被动模式 每次传输都需要重新打开
    if (FTP_DATA_MODE == FTP_PASV_MODE) {
        ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);
    }

    // 传输下载文件指令 RETR
    if (FTPRetr(ftp_ctl_fd, filename) == -1) {
        if (ftp_data_fd != -1) close(ftp_data_fd);
        return -1;
    }

    // 主动模式
    if (FTP_DATA_MODE == FTP_PORT_MODE) {
        ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);
    }

    FTPTransmit(file_handle, ftp_data_fd, recv_buf);

    // 客户端关闭文件和数据套接字
    close(ftp_data_fd);
    close(file_handle);

    // 226 Transfer complete.
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (FTPCheckResponse(recv_buf)) {
        printf("<< Get failed. %s", recv_buf);
        return -1;
    }

    // printf("get ok.\n");
    return 0;
}

int FTPParseCommand(int ftp_ctl_fd, const char* cmd) {
    int flag1, flag2, flag3;
    char cmd_tok[BUFF_SIZE], params1[BUFF_SIZE] = {0}, params2[BUFF_SIZE] = {0};
    int cmd_tok_len = gettoken(cmd, cmd_tok);
    int params1_len = gettoken(cmd + cmd_tok_len + 1, params1);
    int params2_len = gettoken(cmd + cmd_tok_len + params1_len + 1, params2);

    // printf("cmd_tok: %s\nparams1: %s\nparams2: %s\n",
    //        cmd_tok,
    //        params1,
    //        params2);
    switch (*cmd) {
    /* ascii */
    case 'a':
        if (strncmp(cmd_tok, "ascii", 5) != 0) {
            printf("Invalid instruction: %s => ascii ?\n", cmd_tok);
            return -1;
        }
        FTPAscii(ftp_ctl_fd);
        break;
    /* binary */
    case 'b':
        if (strncmp(cmd_tok, "binary", 6) != 0) {
            printf("Invalid instruction: %s => binary ?\n", cmd_tok);
            return -1;
        }
        FTPBinary(ftp_ctl_fd);
        break;
    /* cd */
    case 'c':
        if (strncmp(cmd_tok, "cd", 2) != 0) {
            printf("Invalid instruction: %s => cd ?\n", cmd_tok);
            return -1;
        }

        FTPCd(ftp_ctl_fd, params1);
        break;
    /* delete */
    case 'd':
        if (strncmp(cmd_tok, "delete", 6) != 0) {
            printf("Invalid instruction: %s => delete ?\n", cmd_tok);
            return -1;
        }
        FTPDele(ftp_ctl_fd, params1);
        break;
    /* get */
    case 'g':
        if (strncmp(cmd_tok, "get", 3) != 0) {
            printf("Invalid instruction: %s => get ?\n", cmd_tok);
            return -1;
        }
        FTPGet(ftp_ctl_fd, params1, params2);
        break;
    /* list */
    case 'l':
        if (strncmp(cmd_tok, "ls", 2) != 0) {
            printf("Invalid instruction: %s => ls ?\n", cmd_tok);
            return -1;
        }

        FTPList(ftp_ctl_fd);
        break;
    /* pwd, put, port */
    case 'p':
        if (strncmp(cmd_tok, "pwd", 4) == 0) {
            FTPPwd(ftp_ctl_fd);
            break;
        }
        if (strncmp(cmd_tok, "put", 3) == 0) {
            FTPPut(ftp_ctl_fd, params1, params2);
            break;
        }
        if (strncmp(cmd_tok, "port", 4) == 0) {
            FTPPort(ftp_ctl_fd, params1);
            break;
        }
        if (strncmp(cmd_tok, "pasv", 4) == 0) {
            FTPPasv(ftp_ctl_fd);
            break;
        }

        printf("Invalid instruction: %s => {pwd, put, port, pasv} ?\n",
               cmd_tok);
        break;
    /* mkdir */
    case 'm':
        if (strncmp(cmd_tok, "mkdir", 5) != 0) {
            printf("Invalid instruction: %s => mkdir ?\n", cmd_tok);
            return -1;
        }

        FTPMkdir(ftp_ctl_fd, params1);
        break;
    /* rename, rmdir */
    case 'r':
        if (strncmp(cmd_tok, "rename", 6) == 0) {
            FTPRename(ftp_ctl_fd, params1, params2);
            break;
        }
        if (strncmp(cmd_tok, "rmdir", 5) == 0) {
            FTPRmd(ftp_ctl_fd, params1);
        }

        printf("Invalid instruction: %s => rename or rmdir ?\n", cmd_tok);
        break;
    /* quit */
    case 'q':
        if (strncmp(cmd_tok, "quit", 4) != 0) {
            printf("Invalid instruction: %s => quit ?\n", cmd_tok);
            return -1;
        }
        // quit指令成功就直接退出进程
        if (FTPQuit(ftp_ctl_fd) == 0) {
            exit(EXIT_SUCCESS);
        }
        break;
    /* size, setlimit */
    case 's':
        if (strncmp(cmd_tok, "size", 4) == 0) {
            FTPBinary(ftp_ctl_fd);
            long file_sz = -1;
            if ((file_sz = FTPSize(ftp_ctl_fd, params1)) != -1) {
                printf("%ld\n", file_sz);
            }
            break;
        }
        if (strncmp(cmd_tok, "setlimit", 8) == 0) {
            FTPSetRateLimit(atof(params1));
            break;
        }

        printf("Invalid instruction: %s => size or setlimit ?\n", cmd_tok);
        break;
    default:
        printf("Unknown command.\n");
        return -1;
    }
    return 0;
}

void FTPCommand(int ftp_ctl_fd) {
    write(ftp_ctl_fd, send_buf, strlen(send_buf));
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    printf("<< %s", recv_buf);
}

int FTPConnect(const char* addr, int port) {
    int sock_fd;
    struct sockaddr_in server;
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOGE("socket failed.\n");
        exit(EXIT_FAILURE);
    }

    // 获取服务器IP
    if (addr[0] == 'f') { /* 域名 */
        struct hostent* hp;
        hp = gethostbyname(addr);
        memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
    } else { /* ip地址 */
        server.sin_addr.s_addr = inet_addr(addr);
    }
    strcpy(FTP_SERVER_IP, inet_ntoa(server.sin_addr));
    LOGI("SERVER IP: %s\n", FTP_SERVER_IP);

    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (connect(sock_fd, (struct sockaddr*) &server, sizeof(server)) < 0) {
        LOGE("connect failed.\n");
        exit(EXIT_FAILURE);
    }

    // 获取客户端IP
    struct sockaddr_in client;
    int client_len = sizeof(client);
    if (getsockname(sock_fd,
                    (struct sockaddr*) &client,
                    (socklen_t*) &client_len) < 0) {
        LOGE("getsockname failed.\n");
        exit(EXIT_FAILURE);
    }
    strcpy(FTP_CLIENT_IP, inet_ntoa(client.sin_addr));
    LOGI("CLIENT IP: %s\n", FTP_CLIENT_IP);

    return sock_fd;
}

int FTPLogin(int ftp_ctl_fd, const char* username, const char* password) {
    sprintf(send_buf, "USER %s\r\n", username);
    write(ftp_ctl_fd, send_buf, strlen(send_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (strncmp(recv_buf, "331", 3) != 0) {
        printf("Username not match.\n");
        return -1;
    }

    sprintf(send_buf, "PASS %s\r\n", password);
    write(ftp_ctl_fd, send_buf, strlen(send_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (strncmp(recv_buf, "230", 3) != 0) {
        printf("Password not match.\n");
        return -1;
    }
    return 0;
}

int FTPOpenDataSockfd(int ftp_ctl_fd) {
    if (FTP_DATA_MODE == FTP_PORT_MODE) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        int conn_sock_fd = accept(FTP_DATA_PORT,
                                  (struct sockaddr*) &client_addr,
                                  (socklen_t*) &client_addr_len);
        return conn_sock_fd;
    } else if (FTP_DATA_MODE == FTP_PASV_MODE) {
        FTPPasv(ftp_ctl_fd);
        return FTPConnect(FTP_SERVER_IP, FTP_DATA_PORT);
    }
    return FTPConnect(FTP_SERVER_IP, FTP_DATA_PORT);
}

int main(int argc, const char* argv[]) {
    // 提供IP
    if (argc < 2) {
        LOGE("At least need one paraments.\n");
        exit(EXIT_FAILURE);
    }
    // 提供port
    if (argc >= 3) {
        FTP_PORT = atoi(argv[2]);
    } else {
        FTP_PORT = 21;  // 默认FTP控制端口
    }
    LOGI("FTP Address: %s:%d\n", argv[1], FTP_PORT);
    int ftp_ctl_fd = FTPConnect(argv[1], FTP_PORT);

    // 默认传输模式为 被动模式
    FTP_DATA_MODE = FTP_PASV_MODE;

    // 读取服务器欢迎信息
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);

    char username[BUFF_SIZE], password[BUFF_SIZE];
    while (1) {
        printf("username:");
        scanf("%s", username);
        printf("password:");
        fflushStdin();
        getPassword(password, BUFF_SIZE / 2);
        printf("\n");

        if (FTPLogin(ftp_ctl_fd, username, password) != -1) {
            printf("Login ok.\n");
            break;
        }
    }

    FTPSetRateLimit(-1);  // <=0 不限速
    char cmd[BUFF_SIZE];
    while (1) {
        printf("=> ");
        gets(cmd);
        if (FTPParseCommand(ftp_ctl_fd, cmd) == -1) {
            continue;
        }
        if (strncmp(recv_buf, "421", 3) == 0) {
            printf("Connection broken.\n");
            break;
        }
    }
    return 0;
}