#include <errno.h>
#include <fcntl.h> 
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

// #define DEBUG

int set_interface_attribs(int fd, uint32_t speed, uint8_t parity) {
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "error %d from tcgetattr", errno);
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag |= parity;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "error %d from tcsetattr", errno);
        return -1;
    }
    return 0;
}

uint8_t file_readline(FILE* file, uint8_t* buf) {
    memset(buf, 0, 256);
    uint8_t i = 0;
    int ch = getc(file);
    while ((ch != '\n') && (ch != EOF)) {
        buf[i] = ch;
        ch = getc(file);
        i++;
    }
    return i;
}

uint8_t serial_readline(int serial, uint8_t* buf) {
    memset(buf, 0, 256);
    uint8_t i = 0;
    while (read(serial, &buf[i], 1)) {
        if (buf[i] == '\r') {
            // Do not increment i here and let the next char override it.
            continue;
        }
        if (buf[i] == '\n') {
            // Drop the newline character and make sure the string is NULL
            // terminated.
            buf[i] = 0;
            i++;
            break;
        }
        i++;
    }
    return i;
}

uint8_t* trim_whitespace(uint8_t *str) {
    uint8_t *end;

    while (*str == ' ') {
        str++;
    }

    if (*str == 0) {
        return str;
    }

    end = str + strlen((char*)str) - 1;
    while ((end > str) && (*end == ' ')) {
        end--;
    }

    end[1] = '\0';

    return str;
}

uint16_t sum(uint8_t* line_lengths) {
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 255; i++) {
        if (line_lengths[i] == 0) {
            break;
        }
        sum += line_lengths[i];
    }
    return sum;
}

int usage() {
    printf("Usage: cncstream -d <device> -f <gcode>\n");
    printf("\n");
    printf("    ./cncstream -d /dev/ttyACM0 -f test.gcode\n");
    return 1;
}

int main(int argc, char *argv[]) {
    int opt;

    char portname[256];
    char filename[256];
    memset(portname, 0, 256);
    memset(filename, 0, 256);

    while((opt = getopt(argc, argv, ":f:d:")) != -1) {
        switch(opt) {
            case 'd':
                memcpy(portname, optarg, strlen(optarg));
                break;
            case 'f':
                memcpy(filename, optarg, strlen(optarg));
                break;
            default:
                break;
        }
    }

    if (!strlen(portname) || !strlen(filename)) {
        return usage();
    }

    FILE* file = fopen(filename, "r");
    if (file < 0) {
        fprintf(stderr, "error %d opening %s: %s", errno, portname, strerror(errno));
        return 1;
    }

    int serial = open(portname, O_RDWR|O_NOCTTY|O_SYNC);
    if (serial < 0) {
        fprintf(stderr, "error %d opening %s: %s", errno, portname, strerror(errno));
        fclose(file);
        return 1;
    }
    set_interface_attribs(serial, 115200, 0);

    // Write some wakeup stuff to GRBL and then let it boot. We could look for
    // the GRBL boot string but we might have launched this program way after
    // it booted.
    write(serial, "\r\n\r\n", 4);
    sleep(2);

    // An array of line lengths. When the sum() of these line lengths exceeds
    // the GRBL internal buffer size, we enter into a mode where we wait for
    // "ok" messages to be sent from GRBL to indicate it has processed another
    // line. If so we shift off the line length from the beginning of the line
    // reducing the sum and allowing writes to continue.
    //
    // A shift memmove operation looks like this:
    //
    //     4 items        __ lli == 5
    //                   /
    //   0  1  2  3  4  5  6  7  <-- index of line_lengths[]
    //   xx yy zz aa bb .. .. .. <-- before memmove
    //   yy zz aa bb .. .. .. .. <-- after memmove
    //
    // Moves 5 bytes (including final zero) back towards zero
    // dropping the "xx"
    //
    uint8_t line_lengths[256];
    memset(line_lengths, 0, 256);
    uint8_t lli = 0;

    uint64_t line_count = 0;

    // A write and read buffer. Obviously this program can not handle lines
    // longer than 256 characters. It has the advantage that by using a uint8_t
    // index it's not possible to overflow the buffer.
    uint8_t writebuf[256];
    uint8_t readbuf[256];
    uint8_t read_count = 0;
    int write_count = 0;
    uint16_t sum_line_lengths = 0;

    // While there are still lines in the file:
    while (file_readline(file, writebuf)) {
        // Trim leading and trailing whitespace
        uint8_t* trimmed = trim_whitespace(writebuf);

        if (trimmed[0] == ';') {
            // Don't bother GRBL with GCODE comments
            continue;
        }

        // Append the line length to line_lengths and increment lli to point to
        // the next empty space. Again we are assuming we will never have more
        // than 256 active lines in the buffer in GRBL.
        uint8_t length = strlen((char*)trimmed);
        line_lengths[lli] = length;
        lli++;

        // Calculate the sum of all the line lengths
        sum_line_lengths = sum(line_lengths);

        // Check if the sum of all the line lengths is greater than the GRBL
        // internal buffer size.
        while (sum_line_lengths >= 127) {
            // Do a non-blocking read and look for a line of text.
            read_count = serial_readline(serial, readbuf);

            // If something was read ("ok" for example) then shift off the
            // first item in line_lengths. This should reduce sum(line_lengths)
            // back to under the internal buffer size.
            if (read_count > 0) {
#ifdef DEBUG
                fprintf(stderr, "'%s'\n", readbuf);
#endif
                memmove(&line_lengths[0], &line_lengths[1], lli);
                lli--;
                sum_line_lengths = sum(line_lengths);
            }
        }

#ifdef DEBUG
        fprintf(stderr, "%s\n", trimmed);
#endif
        // Append a newline and write to the serial device. Does the newline
        // contribute to the GRBL buffer? I don't know but this code assumes
        // no.
        trimmed[length] = '\n';
        write_count = write(serial, trimmed, length + 1);
        if (write_count != (length + 1)) {
            fprintf(stderr, "ERROR: wrote a different amount\n");
        }
        read_count = serial_readline(serial, readbuf);
        if (read_count > 0) {
            memmove(&line_lengths[0], &line_lengths[1], lli);
            lli--;
        }
        line_count++;
    }

    // Wait to receive the "ok"s for the items still in the GRBL buffer.
    // However we obviously get out of sync because lli never seems to reach
    // zero. Either I wasn't patient enough to wait for the final "ok"s to come
    // back. I am pretty sure it's just that GRBL sent some "ok"s and we wer
    // enot ready for them and missed them somehow.
    while (lli > 2) {
        read_count = serial_readline(serial, readbuf);
        if (read_count > 0) {
            memmove(&line_lengths[0], &line_lengths[1], lli);
            lli--;
#ifdef DEBUG
            fprintf(stderr, "'%s' (lli: %d)\n", readbuf, lli);
#endif
        }
    }

    return 0;
}
