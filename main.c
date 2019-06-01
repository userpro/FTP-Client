/*
    FTP指令: http://www.nsftools.com/tips/RawFTP.htm
    支持指令
    cd, list, pwd, mkdir, put, get, setlimit,
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

#define FTP_PORT 2121
#define BUFF_SIZE 1024
static const char* FTP_IP;
static int FTP_BYTES_PER_SEC;  // 流量控制, 每second多少byte

typedef struct {
    char cmd[BUFF_SIZE];
    char params[BUFF_SIZE];
} Cmder;

/* 工具函数 */
int find(const char* str, const char* pattern);
int parsePath(const char* path, char* dirname, char* filename);
int gettoken(const char* src, char* res);
void fflushStdin();
int getPassword(char* password, int size);
const char* skipResponseCode(const char* response);

/* FTP 指令 */
int FTPPasv(int ftp_ctl_fd);
int FTPPort(int ftp_ctl_fd, int p1, int p2);

int FTPRest(int ftp_ctl_fd, long int offset);
int FTPStor(int ftp_ctl_fd, const char* filename);
int FTPRetr(int ftp_ctl_fd, const char* filename);
int FTPCd(int ftp_ctl_fd, const char* path);
int FTPList(int ftp_ctl_fd, const char* path);
int FTPPwd(int ftp_ctl_fd);
int FTPMkdir(int ftp_ctl_fd, const char* dirname);
int FTPSize(int ftp_ctl_fd, const char* filename);
int FTPDele(int ftp_ctl_fd, const char* path);
int FTPRmd(int ftp_ctl_fd, const char* dirname);
int FTPRename(int ftp_ctl_fd, const char* path, const char* newpath);
int FTPAscii(int ftp_ctl_fd);
int FTPBinary(int ftp_ctl_fd);
int FTPQuit(int ftp_ctl_fd);

/* FTP 操作 */
void FTPSetRateLimit(double ftp_rate_limit_kb);
void FTPCommand(int ftp_ctl_fd);
int FTPTransmit(int dest_fd, int src_fd, void* trans_buf);
int FTPGet(int ftp_ctl_fd, const char* path, const char* newpath);
int FTPPut(int ftp_ctl_fd, const char* path, const char* newpath);
int FTPConnect(const char* addr, int port);
int FTPOpenDataSockfd(int ftp_ctl_fd);
int FTPParseCommand(int ftp_ctl_fd, const char* cmd);
int FTPLogin(int ftp_ctl_fd, const char* username, const char* password);

/* 数据缓冲区 */
static char recv_buf[BUFF_SIZE], send_buf[BUFF_SIZE];

/* ---------------------------------- */

int find(const char* str, const char* pattern) {
    const char* src = str;
    int len = strlen(pattern);
    while (*src != '\0') {
        if (*src == *pattern && strncmp(src, pattern, len) == 0) return 0;
        src++;
    }
    return -1;
}

int parsePath(const char* path, char* dirname, char* filename) {
    int i;
    int path_len = strlen(path);
    for (i = path_len - 1; i >= 0; i--) {
        if (path[i] == '/' || path[i] == '\\') break;
    }
    strcpy(filename, path + i + 1);
    if (i >= 0) {
        strncpy(dirname, path, i);
        dirname[i] = '\0';
    }
    return 0;
}

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
    响应 "350 Restarting at <position>. Send STORE or RETRIEVE to initiate
   transfer."
*/
int FTPRest(int ftp_ctl_fd, long int offset) {
    sprintf(send_buf, "REST %ld\r\n", offset);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "350", 3) != 0) {
        printf("REST failed. >> %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "PASV\r\n"
    响应 "227 Command okay."
    客户端发送命令改变FTP数据模式为被动模式
*/
int FTPPasv(int ftp_ctl_fd) {
    sprintf(send_buf, "PASV\r\n");
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "227", 3) != 0) {
        printf("PASV failed.\n");
        return -1;
    }
    int port = -1, h1, h2, h3, h4, p1, p2;
    sscanf(recv_buf, "%*[^(](%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2);
    port = p1 * 256 + p2;
    return port;
}

/*
    命令 "PORT a1,a2,a3,a4,p1,p2\r\n"
    客户端发送命令改变FTP数据模式为主动模式
*/
int FTPPort(int ftp_ctl_fd, int p1, int p2) {
    int h1, h2, h3, h4;
    sscanf(FTP_IP, "%d.%d.%d.%d", &h1, &h2, &h3, &h4);
    sprintf(send_buf, "PORT %d,%d,%d,%d,%d,%d\r\n", h1, h2, h3, h4, p1, p2);
    FTPCommand(ftp_ctl_fd);
    LOGI("%s", recv_buf);
    return 0;
}

/*
    命令 "STOR filename\r\n"
    响应 "125 Data connection already open. Transfer starting."
    客户端发送命令上传文件至服务器端
*/
int FTPStor(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "STOR %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "125", 3) != 0) {
        printf("STOR failed. >> %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "RETR filename\r\n"
    响应 "125 Data connection already open. Transfer starting."
    客户端发送命令从服务器端下载文件
*/
int FTPRetr(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "RETR %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "125", 3) != 0) {
        printf("RETR failed. >> %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "CWD dirname\r\n"
    客户端发送命令改变工作目录
    客户端接收服务器的响应码和信息
    正常为 "250 Command okay."
*/
int FTPCd(int ftp_ctl_fd, const char* dirname) {
    sprintf(send_buf, "CWD %s\r\n", dirname);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "250", 3) != 0) {
        printf("CWD failed. >> %s", recv_buf);
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
int FTPList(int ftp_ctl_fd, const char* path) {
    // 打开数据传输套接字
    int ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);

    sprintf(send_buf, "LIST %s\r\n", path);
    FTPCommand(ftp_ctl_fd);
    // 125 Data connection already open. Transfer starting.
    if (strncmp(recv_buf, "125", 3) != 0) {
        printf("LIST failed. >> %s", recv_buf);
        close(ftp_data_fd);
        return -1;
    }

    // read data
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_data_fd, recv_buf, BUFF_SIZE);
    printf("%s", recv_buf);
    // 关闭数据套接字
    close(ftp_data_fd);

    // 226 Transfer complete.
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (strncmp(recv_buf, "226", 3) != 0) {
        printf("LIST failed. [%s]", recv_buf);
        return -1;
    }

    // printf("LIST %s ok.\n", path);
    return 0;
}

/*
    命令 "pwd\r\n"
    客户端发送命令获取当前所在路径
    客户端接收服务器的响应码和信息
    正常为 "257 "%s" is the current directory."
*/
int FTPPwd(int ftp_ctl_fd) {
    sprintf(send_buf, "PWD\r\n");
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "257", 3) != 0) {
        printf("PWD failed. >> %s", recv_buf);
        return -1;
    }
    // 去除开头的response code
    printf("%s", skipResponseCode(recv_buf));
    // printf("PWD ok.\n");
    return 0;
}

/*
    命令 "MKD dirname\r\n"
    响应 "257 \"%s\" directory created."
    创建目录
*/
int FTPMkdir(int ftp_ctl_fd, const char* dirname) {
    sprintf(send_buf, "MKD %s\r\n", dirname);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "257", 3) != 0) {
        printf("MKD failed. >> %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "SIZE filename\r\n"
    客户端发送命令从服务器端得到下载文件的大小
    客户端接收服务器的响应码和信息，正常为 "213 <size>"
*/
int FTPSize(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "SIZE %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "213", 3) != 0) {
        printf("SIZE failed. >> %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "DELE filename\r\n"
    响应 "250 File removed."
    创建目录
*/
int FTPDele(int ftp_ctl_fd, const char* filename) {
    sprintf(send_buf, "DELE %s\r\n", filename);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "250", 3) != 0) {
        printf("DELE failed. >> %s", recv_buf);
        return -1;
    }
    return 0;
}

/*
    命令 "RMD dirname\r\n"
    响应 "250 Directory removed."
    创建目录
*/
int FTPRmd(int ftp_ctl_fd, const char* dirname) {
    sprintf(send_buf, "RMD %s\r\n", dirname);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "250", 3) != 0) {
        printf("RMD failed. >> %s", recv_buf);
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
int FTPRename(int ftp_ctl_fd, const char* path, const char* newfilename) {
    char dirname[BUFF_SIZE], oldfilename[BUFF_SIZE];
    parsePath(path, dirname, oldfilename);

    if (FTPCd(ftp_ctl_fd, dirname) == -1) {
        return -1;
    }

    sprintf(send_buf, "RNFR %s\r\n", oldfilename);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "350", 3) != 0) {
        printf("RNFR failed. >> %s", recv_buf);
        return -1;
    }
    sprintf(send_buf, "RNTO %s\r\n", newfilename);
    FTPCommand(ftp_ctl_fd);
    if (strncmp(recv_buf, "250", 3) != 0) {
        printf("RNTO failed. >> %s", recv_buf);
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
    if (strncmp(recv_buf, "200", 3) != 0) {
        printf("TYPE A failed. >> %s", recv_buf);
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
    if (strncmp(recv_buf, "200", 3) != 0) {
        printf("TYPE I failed. >> %s", recv_buf);
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
    if (strncmp(recv_buf, "221", 3) != 0) {
        printf("QUIT failed. >> %s", recv_buf);
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
            sleep(1);
            nx_time = time(NULL);
            limit_bytes = (nx_time - cur_time) * FTP_BYTES_PER_SEC;
            limit_bytes *= 2;  // 补偿 sleep 1 sec
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
                LOGE("write error in get.\n");
            }
            nleft -= nread;
        }
        total_trans_bytes += limit_bytes - nleft;
        printf("Alreay transmitted %lld bytes\n", total_trans_bytes);
        if (flag) {
            break;
        }
    }
    return 0;
}

int FTPPut(int ftp_ctl_fd, const char* path, const char* newpath) {
    // 检查本地文件是否存在
    if (access(path, F_OK) < 0) {
        printf("%s [No such file or directory.]\n", path);
        return -1;
    }

    char dirname[BUFF_SIZE], filename[BUFF_SIZE];
    // 解析出目标路径和目标文件名
    parsePath(newpath, dirname, filename);

    if (FTPCd(ftp_ctl_fd, dirname) == -1) {
        return -1;
    }

    // 未给出上传后新的文件名则使用源文件名
    if (strlen(filename) == 0) {
        parsePath(path, dirname, filename);
    }

    // 连接服务器新开的数据端口
    int ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);

    // 开启binary模式
    if (FTPBinary(ftp_ctl_fd) == -1) {
        return -1;
    }

    // 传输上传文件指令 STOR
    if (FTPStor(ftp_ctl_fd, filename) == -1) {
        close(ftp_data_fd);
        return -1;
    }

    /* 客户端打开文件 */
    int file_handle = open(path, O_RDONLY, 0);

    FTPTransmit(ftp_data_fd, file_handle, send_buf);

    /* 关闭数据传输套接字 */
    close(ftp_data_fd);
    /* 客户端关闭文件 */
    close(file_handle);

    // 226 Transfer complete.
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    LOGI("%s", recv_buf);
    if (strncmp(recv_buf, "226", 3) != 0) {
        printf("STOR failed. >> %s", recv_buf);
        return -1;
    }

    // printf("put ok.\n");
    return 0;
}

int FTPGet(int ftp_ctl_fd, const char* path, const char* newfilename) {
    char dirname[BUFF_SIZE], filename[BUFF_SIZE];
    parsePath(path, dirname, filename);
    // LOGI("%s %s %s\n", path, dirname, filename);

    // 开启binary模式
    if (FTPBinary(ftp_ctl_fd) == -1) {
        return -1;
    }

    if (FTPCd(ftp_ctl_fd, dirname) == -1) {
        return -1;
    }
    if (FTPSize(ftp_ctl_fd, filename) == -1) {
        return -1;
    }
    int ftp_file_size = -1;
    sscanf(skipResponseCode(recv_buf), "%d", &ftp_file_size);

    // 连接服务器新开的数据端口
    int ftp_data_fd = FTPOpenDataSockfd(ftp_ctl_fd);

    // 下载文件是否重命名
    const char* name = newfilename;
    if (strlen(name) == 0) {
        name = filename;
    }

    // 如果文件存在 断点续传
    int file_handle = -1;
    if (access(name, F_OK) == 0) {
        long int offset = 0;
        file_handle = open(name, O_WRONLY | O_APPEND);
        int err = 0;  // 错误标示
        if ((offset = lseek(file_handle, 0, SEEK_END)) != -1) {
            if (offset == ftp_file_size) {
                printf("File exists.\n");
                err = 1;
            }
            // 如果断点续传失败 则取消下载
            if (!err && FTPRest(ftp_ctl_fd, offset) == -1) {
                printf("Resume from break-point failed.\n");
                err = 1;
            }
        } else {
            err = 1;
            LOGE("lseek failed.\n");
        }
        if (err) {
            close(ftp_data_fd);
            close(file_handle);
            return -1;
        }
    } else {
        file_handle = open(name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    }

    // 传输下载文件指令 RETR
    if (FTPRetr(ftp_ctl_fd, filename) == -1) {
        close(ftp_data_fd);
        return -1;
    }

    FTPTransmit(file_handle, ftp_data_fd, recv_buf);

    // 客户端关闭文件和数据套接字
    close(ftp_data_fd);
    close(file_handle);

    // 226 Transfer complete.
    memset(recv_buf, 0, sizeof(recv_buf));
    read(ftp_ctl_fd, recv_buf, BUFF_SIZE);
    // LOGI("%s", recv_buf);
    if (strncmp(recv_buf, "226", 3) != 0) {
        printf("RETR failed. >> %s", recv_buf);
        return -1;
    }

    // printf("get ok.\n");
    return 0;
}

int FTPParseCommand(int ftp_ctl_fd, const char* cmd) {
    int flag1, flag2;
    char cmd_tok[BUFF_SIZE], params1[BUFF_SIZE] = {0}, params2[BUFF_SIZE] = {0};
    int cmd_tok_len = gettoken(cmd, cmd_tok);
    int params1_len = gettoken(cmd + cmd_tok_len + 1, params1);
    int params2_len = gettoken(cmd + cmd_tok_len + params1_len + 1, params2);
    // printf("cmd_tok: %s\nparams1: %s\nparams2: %s\n",
    //        cmd_tok,
    //        params1,
    //        params2);
    switch (*cmd) {
    /* cd */
    case 'c':
        if (strncmp(cmd_tok, "cd", 2) != 0) {
            printf("Invalid instruction: %s => cd ?\n", cmd_tok);
            return -1;
        }
        FTPCd(ftp_ctl_fd, params1);
        break;
    /* list */
    case 'l':
        if (strncmp(cmd_tok, "list", 4) != 0) {
            printf("Invalid instruction: %s => list ?\n", cmd_tok);
            return -1;
        }
        FTPList(ftp_ctl_fd, params1);
        break;
    /* pwd, put */
    case 'p':
        flag1 = 1, flag2 = 1;
        if (strncmp(cmd_tok, "pwd", 4) != 0) {
            flag1 = 0;
        }
        if (strncmp(cmd_tok, "put", 3) != 0) {
            flag2 = 0;
        }
        if (!flag1 && !flag2) {
            printf("Invalid instruction: %s => pwd or put ?\n", cmd_tok);
            return -1;
        }

        if (flag1) {
            FTPPwd(ftp_ctl_fd);
        }
        if (flag2) { /* put */
            FTPPut(ftp_ctl_fd, params1, params2);
        }
        break;
    /* mkdir */
    case 'm':
        if (strncmp(cmd_tok, "mkdir", 5) != 0) {
            printf("Invalid instruction: %s => mkdir ?\n", cmd_tok);
            return -1;
        }
        FTPMkdir(ftp_ctl_fd, params1);
        break;
    /* get */
    case 'g':
        if (strncmp(cmd_tok, "get", 3) != 0) {
            printf("Invalid instruction: %s => get ?\n", cmd_tok);
            return -1;
        }
        FTPGet(ftp_ctl_fd, params1, params2);
        break;
    /* delete */
    case 'd':
        if (strncmp(cmd_tok, "delete", 6) != 0) {
            printf("Invalid instruction: %s => delete ?\n", cmd_tok);
            return -1;
        }
        FTPDele(ftp_ctl_fd, params1);
        break;
    /* rename, rmdir */
    case 'r':
        flag1 = 1, flag2 = 1;
        if (strncmp(cmd_tok, "rename", 6) != 0) {
            flag1 = 0;
        }
        if (strncmp(cmd_tok, "rmdir", 5) != 0) {
            flag2 = 0;
        }
        if (!flag1 && !flag2) {
            printf("Invalid instruction: %s => rename ?\n", cmd_tok);
            return -1;
        }

        if (flag1) { /* rename */
            FTPRename(ftp_ctl_fd, params1, params2);
        }
        if (flag2) { /* remove dir */
            FTPRmd(ftp_ctl_fd, params1);
        }
        break;
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
    case 's':
        if (strncmp(cmd_tok, "setlimit", 8) != 0) {
            printf("Invalid instruction: %s => setlimit ?\n", cmd_tok);
            return -1;
        }
        FTPSetRateLimit(atof(params1));
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
    LOGI("%s", recv_buf);
}

int FTPConnect(const char* addr, int port) {
    int sock_fd;
    struct sockaddr_in server;
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOGE("socket failed.\n");
        exit(EXIT_FAILURE);
    }

    if (addr[0] == 'f') { /* 域名 */
        struct hostent* hp;
        hp = gethostbyname(addr);
        memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
    } else { /* ip地址 */
        server.sin_addr.s_addr = inet_addr(addr);
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(port);

    if (connect(sock_fd, (struct sockaddr*) &server, sizeof(server)) < 0) {
        LOGE("connect failed.\n");
        exit(EXIT_FAILURE);
    }

    // setNonblock(sock_fd);

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
    int data_port;
    while ((data_port = FTPPasv(ftp_ctl_fd)) == -1) {
        sleep(1000);
    }
    return FTPConnect(FTP_IP, data_port);
}

int main(int argc, const char* argv[]) {
    if (argc < 2) {
        LOGE("Need one paraments.\n");
        exit(EXIT_FAILURE);
    }
    FTP_IP = argv[1];
    int ftp_ctl_fd = FTPConnect(FTP_IP, FTP_PORT);

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
            // printf("Login ok.\n");
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