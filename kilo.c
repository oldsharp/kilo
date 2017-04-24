#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define KILO_VERSION "0.0.1"

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
	/*
	 * cx is the horizontal coordinate of the cursor (the column).
	 * cy is the vertical coordinate (the row).
	 */
	int cx;
	int cy;
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editor_config E;

void die(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disable_raw_mode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enable_raw_mode()
{
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disable_raw_mode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag &= ~(CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

char editor_read_key()
{
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
		if (nread == -1 && errno != EAGAIN)
			die("read");

	return c;
}

int get_cursor_position(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;

	/*
	 * The n command (Device Status Report) can be used to query
	 * the terminal for status information.  We give it an argument
	 * of 6 to ask for the cursor position.  Then we can read the
	 * reply from STDIN.
	 */
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	return 0;
}

int get_window_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		/*
		 * The C command (Cursor Forward) moves the cursor to
		 * the right, and the B command (Cursor Down) moves the
		 * cursor down.  The large argument 999 should ensure
		 * that the cursor reaches the right and bottom edges
		 * of the screen.
		 */
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return get_cursor_position(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*
 * Append buffer.
 */
struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void ab_append(struct abuf *ab, const char *s, int len)
{
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL)
		return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void ab_free(struct abuf *ab)
{
	free(ab->b);
}

void editor_draw_rows(struct abuf *ab)
{
	int i;
	for (i = 0; i < E.screenrows; i++) {
		if (i == E.screenrows / 3) {
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome),
				"Kilo editor -- version %s", KILO_VERSION);
			if (welcomelen > E.screencols)
				welcomelen = E.screencols;
			int padding = (E.screencols - welcomelen) / 2;
			if (padding) {
				ab_append(ab, "~", 1);
				padding--;
			}
			while (padding--)
				ab_append(ab, " ", 1);
			ab_append(ab, welcome, welcomelen);
		} else {
			ab_append(ab, "~", 1);
		}
		/*
		 * The K command (Erase In Line) erases part of the
		 * current line.  Its argument is analogous to the J
		 * command's argument: 2 erases the whole line,
		 * 1 erases the part of the line to the left of the
		 * cursor, and 0 erases the part of the line to the
		 * right of the cursor.  0 is the default argument,
		 * and that's what we want, so we leave out the
		 * argument and just use <esc>[K.
		 */
		ab_append(ab, "\x1b[K", 3);
		if (i < E.screenrows - 1)
			ab_append(ab, "\r\n", 2);
	}
}

void editor_refresh_screen()
{
	struct abuf ab = ABUF_INIT;

	/*
	 * The l and h commands in the escape sequences below are used
	 * to tell the terminal to hide and show the cursor.  Note that
	 * some terminals might not support hiding/showing the cursor,
	 * because the argument "?25" appeared in later VT models, not
	 * VT100.
	 */
	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy+1, E.cx+1);
	ab_append(&ab, buf, strlen(buf));
	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void editor_process_keypress()
{
	char c = editor_read_key();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
	}
}

void init_editor()
{
	E.cx = 0;
	E.cy = 0;

	if (get_window_size(&E.screenrows, &E.screencols) == -1)
		die("get_window_size");
}

int main()
{
	enable_raw_mode();
	init_editor();

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
