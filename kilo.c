#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*
 * This mirrors what the CTRL key does in the terminal: it strips the
 * 6th and 7th bits from whatever key you press in combination with
 * CTRL, and sends that.  For example:
 *
 * a		0061	0110 0001
 * CTRL-a	0001	0000 0001
 *
 * q		0071	0111 0001
 * CTRL-q	0011	0001 0001
 */
#define CTRL_KEY(k) ((k) & 0x1f)

struct editor_config {
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editor_config E;

void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disable_raw_mode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
		die("tcsetattr");
	}
}

void enable_raw_mode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
		die("tcgetattr");
	}
	atexit(disable_raw_mode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		die("tcsetattr");
	}
}

char editor_read_key() {
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) {
			die("read");
		}
	}

	return c;
}

int get_window_size(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		return -1;
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

void editor_draw_rows() {
	int i;
	for (i = 0; i < 24; i++) {
		write(STDOUT_FILENO, "~\r\n", 3);
	}
}

void editor_refresh_screen() {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	editor_draw_rows();

	write(STDOUT_FILENO, "\x1b[H", 3);
}

void editor_process_keypress() {
	char c = editor_read_key();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
	}
}

void init_editor() {
	if (get_window_size(&E.screenrows, &E.screencols) == -1) {
		die("get_window_size");
	}
}

int main() {
	enable_raw_mode();
	init_editor();

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
