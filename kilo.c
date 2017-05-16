#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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

/*
 * We need to choose a representation for arrow keys.  We give them a
 * large integer (1000~1003) that is out of the range of a char, so
 * that they don't conflict with any ordinary keypresses.
 */
enum editor_key {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

typedef struct erow {
	int size;
	char *chars;
} erow;

struct editor_config {
	/*
	 * cx is the horizontal coordinate of the cursor (the column).
	 * cy is the vertical coordinate of the cursor (the row).
	 */
	int cx;
	int cy;

	/*
	 * Row and column offset.
	 */
	int rowoff;
	int coloff;

	/*
	 * Screen boundary: how many rows and columns the screen
	 * allows to display.
	 */
	int screenrows;
	int screencols;

	/*
	 * Row info for current screen.
	 */
	int numrows;
	erow *row;

	/*
	 * Original terminal configurations.  Used to recovery to the
	 * initial state when exit.
	 */
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
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
		die("tcsetattr");
	}
}

void enable_raw_mode()
{
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

int editor_read_key()
{
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) {
			die("read");
		}
	}

	/*
	 * Detect the arrow keys.  Arrow keys are sent in the form of
	 * an escape sequence that starts with '\x1b', '[', followed
	 * by an 'A', 'B', 'C', or 'D'.
	 *
	 * Also detect Page-Up and Page-Down keys.  Page-Up is sent as
	 * <esc>[5~ and Page-Down is sent as <esc>[6~.
	 *
	 * Also detect Home-Key and End-Key.  The Home-Key could be
	 * sent as <esc>[1~, <esc>[7~, <esc>[H, or <esc>OH.  The
	 * End-Key could be sent as <esc>[4~, <esc>[8~, <esc>[F, or
	 * <esc>OF.  What escape sequence we receive is depending on
	 * the OS type or the terminal emulator used.  Here we handle
	 * all of these possible cases.
	 *
	 * Also detect Del-Key.  The Del-Key is sent as <esc>[3~.
	 */
	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) {
			return '\x1b';
		}
		if (read(STDIN_FILENO, &seq[1], 1) != 1) {
			return '\x1b';
		}

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) {
					return '\x1b';
				}
				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1':
							return HOME_KEY;
						case '3':
							return DEL_KEY;
						case '4':
							return END_KEY;
						case '5':
							return PAGE_UP;
						case '6':
							return PAGE_DOWN;
						case '7':
							return HOME_KEY;
						case '8':
							return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A':
						return ARROW_UP;
					case 'B':
						return ARROW_DOWN;
					case 'C':
						return ARROW_RIGHT;
					case 'D':
						return ARROW_LEFT;
					case 'H':
						return HOME_KEY;
					case 'F':
						return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H':
					return HOME_KEY;
				case 'F':
					return END_KEY;
			}
		}
		return '\x1b';
	} else {
		return c;
	}
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
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
		return -1;
	}

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) {
			break;
		}
		if (buf[i] == 'R') {
			break;
		}
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') {
		return -1;
	}
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
		return -1;
	}

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
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
			return -1;
		}
		return get_cursor_position(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

void editor_append_row(char *s, size_t len)
{
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.numrows++;
}

/*
 * File I/O.
 */
void editor_open(char *filename)
{
	FILE *fp = fopen(filename, "r");
	if (!fp) {
		die("fopen");
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 &&
				(line[linelen - 1] == '\n' ||
				line[linelen - 1] == '\r')) {
			linelen--;
		}
		editor_append_row(line, linelen);
	}
	free(line);
	fclose(fp);
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

	if (new == NULL) {
		return;
	}
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void ab_free(struct abuf *ab)
{
	free(ab->b);
}

void editor_scroll()
{
	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}
}

void editor_draw_rows(struct abuf *ab)
{
	int i;
	for (i = 0; i < E.screenrows; i++) {
		int filerow = i + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows == 0 && i == E.screenrows / 3) {
				char welcome[80];
				int welcomelen = snprintf(
					welcome,
					sizeof(welcome),
					"Kilo editor -- version %s",
					KILO_VERSION);
				if (welcomelen > E.screencols) {
					welcomelen = E.screencols;
				}
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					ab_append(ab, "~", 1);
					padding--;
				}
				while (padding--) {
					ab_append(ab, " ", 1);
				}
				ab_append(ab, welcome, welcomelen);
			} else {
				ab_append(ab, "~", 1);
			}
		} else {
			int len = E.row[filerow].size - E.coloff;
			/*
			 * Note that when subtracting E.coloff from the
			 * length, len can now be a negative number,
			 * meaning the user scrolled horizontally past
			 * the end of the line.  In that case, we set
			 * len to 0 so that nothing is displayed on
			 * that line.
			 */
			if (len < 0) {
				len = 0;
			}
			if (len > E.screencols) {
				len = E.screencols;
			}
			ab_append(ab, &E.row[filerow].chars[E.coloff], len);
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
		if (i < E.screenrows - 1) {
			ab_append(ab, "\r\n", 2);
		}
	}
}

void editor_refresh_screen()
{
	editor_scroll();

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
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
			E.cy-E.rowoff+1,
			E.cx-E.coloff+1);
	ab_append(&ab, buf, strlen(buf));
	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void editor_move_cursor(int key)
{
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			} else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows) {
				E.cy++;
			}
			break;
	}

	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
}

void editor_process_keypress()
{
	int c = editor_read_key();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			E.cx = E.screencols - 1;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screenrows;
				while (times--) {
					editor_move_cursor(c == PAGE_UP ?
							ARROW_UP : ARROW_DOWN);
				}
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editor_move_cursor(c);
			break;
	}
}

void init_editor()
{
	E.cx = 0;
	E.cy = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;

	if (get_window_size(&E.screenrows, &E.screencols) == -1) {
		die("get_window_size");
	}
}

int main(int argc, char *argv[])
{
	enable_raw_mode();
	init_editor();

	if (argc >= 2) {
		editor_open(argv[1]);
	}

	while (1) {
		editor_refresh_screen();
		editor_process_keypress();
	}

	return 0;
}
