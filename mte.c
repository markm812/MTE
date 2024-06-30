/*** includes ***/
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
#include <sys/param.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define MTE_VERSION "0.0.1"
#define TAB_STOP 8

enum EditorKey
{
	ARROW_LEFT = 1001,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	PAGE_UP,
	PAGE_DOWN,
	HOME_KEY,
	END_KEY,
};

/*** data ***/
typedef struct EditorRow
{
	int size;
	int rsize;
	char *chars;
	char *render;
} EditorRow;

struct EditorConfig
{
	int rowOffset, columnOffset;
	int screenRows, screenColumns;
	int cursorX, cursorY, cursorXS;
	int renderX;
	int numRows;
	EditorRow *row;
	struct termios oldtio;
} EC;

/*** terminal ***/
void terminate(const char *s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode()
{
	// Reset raw mode & input leftover will be discarded
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EC.oldtio) == -1)
		terminate("[Error] tcsetattr");
}

void enableRawMode()
{
	// Get current parameters & register exit recovery
	if (tcgetattr(STDIN_FILENO, &EC.oldtio) == -1)
		terminate("[Error] tcgetattr");
	atexit(disableRawMode);

	struct termios newtio = EC.oldtio;
	newtio.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	newtio.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	newtio.c_cflag |= (CS8);
	newtio.c_oflag &= ~(OPOST);
	newtio.c_cc[VMIN] = 0;
	newtio.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtio) == -1)
		terminate("[Error] tcsetattr");
}

int editorReadKey()
{
	ssize_t size;
	char c;
	while ((size = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (size == -1 && errno != EAGAIN)
			terminate("[Error] read");
	}

	// ESC keys
	if (c == '\x1b')
	{
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
			return '\x1b';

		if (seq[0] == '[')
		{
			if (seq[1] >= '0' && seq[1] <= '9')
			{
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if (seq[2] == '~')
				{
					switch (seq[1])
					{
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
			}
			else
			{
				switch (seq[1])
				{
				case 'A':
					return ARROW_UP;
				case 'B':
					return ARROW_DOWN;
				case 'C':
					return ARROW_RIGHT;
				case 'D':
					return ARROW_LEFT;
				case 'F':
					return END_KEY;
				case 'H':
					return HOME_KEY;
				}
			}
		}
		else if (seq[0] == 'O')
		{
			switch (seq[1])
			{
			case 'F':
				return END_KEY;
			case 'H':
				return HOME_KEY;
			}
		}
		return '\x1b';
	}
	return c;
}

void editorExit()
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	exit(0);
}

int getCursorPosition(int *rows, int *cols)
{
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	char buf[32];
	unsigned int i = 0;

	while (i < sizeof(buf) - 1)
	{
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		++i;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;

	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;

	// ioctl will place screen size values into ws on success
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
	{
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
		return -1;
	}

	*rows = ws.ws_row;
	*cols = ws.ws_col;

	return 0;
}

/*** Row operations ***/
int editorRowCursorXToRenderX(EditorRow *row, int cursorX)
{
	int renderX = 0;
	int j;
	for (j = 0; j < cursorX; ++j)
	{
		if (row->chars[j] == '\t')
		{
			renderX += (TAB_STOP - 1) - (renderX % TAB_STOP); // find next tab column
		}
		++renderX;
	}

	return renderX;
}

void editorUpdateRow(EditorRow *row)
{
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t')
			tabs++;

	free(row->render);
	// each tab is 8 chars so +7 per tab
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++)
	{
		if (row->chars[j] == '\t')
		{
			row->render[idx++] = ' ';
			while (idx % TAB_STOP != 0)
				row->render[idx++] = ' ';
		}
		else
		{
			row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *s, size_t len)
{
	EC.row = realloc(EC.row, sizeof(EditorRow) * (EC.numRows + 1));

	int at = EC.numRows;
	EC.row[at].size = len;
	EC.row[at].chars = malloc(len + 1);
	memcpy(EC.row[at].chars, s, len);
	EC.row[at].chars[len] = '\0';

	EC.row[at].rsize = 0;
	EC.row[at].render = NULL;
	editorUpdateRow(&EC.row[at]);

	EC.numRows++;
}

/*** file IO ***/
void editorOpen(const char *filename)
{
	FILE *fp = fopen(filename, "r");
	if (!fp)
		terminate("[Error] fopen");

	char *line = NULL;
	ssize_t lineLen = 0;
	size_t size = 0;
	while ((lineLen = getline(&line, &size, fp)) != -1)
	{
		// strip next line char
		while (lineLen > 0 && (line[lineLen - 1] == '\n' ||
							   line[lineLen - 1] == '\r'))
			lineLen--;

		editorAppendRow(line, lineLen);
	}
	free(line);
	fclose(fp);
}

/*** buffer append ***/
struct abuf
{
	char *b;
	int len;
};

#define ABUF_INIT \
	{             \
		NULL, 0   \
	}

void abAppend(struct abuf *ab, const char *s, int len)
{
	char *newBuf = realloc(ab->b, ab->len + len);
	if (newBuf == NULL)
		return;

	memcpy(&newBuf[ab->len], s, len);
	ab->b = newBuf;
	ab->len += len;
}

void abFree(struct abuf *ab)
{
	free(ab->b);
}

/*** input ***/

// Todo: optimize, track the last direction operation and update realX in to RenderX function
int editorRealCursorXOnLineWithTabs(EditorRow *row, int cursorX)
{
	int realX = 0;
	int j;
	for (j = 0; j < cursorX; ++j)
	{
		realX += (row->chars[j] == '\t') ? (TAB_STOP - (realX % TAB_STOP)) : 1;
		if (realX > cursorX)
		{
			realX = j;
			break;
		}
	}
	return realX;
}

void editorMoveCursorLeft()
{
	if (EC.cursorX > 0)
	{
		EC.cursorX--;
		EC.cursorXS = EC.cursorX;
	}
	else if (EC.cursorY > 0)
	{
		EC.cursorY--;
		EC.cursorX = EC.row[EC.cursorY].size;
		EC.cursorXS = EC.cursorX;
	}
}

void editorMoveCursorRight()
{
	if (EC.cursorY < EC.numRows)
	{
		if (EC.cursorX < EC.row[EC.cursorY].size)
		{
			EC.cursorX++;
			EC.cursorXS = EC.cursorX;
			return;
		}

		EC.cursorY++;
		EC.cursorX = 0;
		EC.cursorXS = EC.cursorX;
	}
}

void editorMoveCursorUp()
{
	if (EC.cursorY > 0)
	{
		EC.cursorY--;
		EC.cursorX = EC.renderX;
	}
}

void editorMoveCursorDown()
{
	if (EC.cursorY < EC.numRows - 1)
	{
		EC.cursorY++;
		EC.cursorX = EC.renderX;
	}
}

void editorMoveCursor(int direction)
{
	switch (direction)
	{
	case ARROW_LEFT:
		editorMoveCursorLeft();
		break;
	case ARROW_RIGHT:
		editorMoveCursorRight();
		break;
	case ARROW_UP:
		editorMoveCursorUp();
		break;
	case ARROW_DOWN:
		editorMoveCursorDown();
		break;
	}

	int rowLen = (EC.cursorY >= EC.numRows) ? 0 : EC.row[EC.cursorY].size;
	// back to the last cursor X before the snapping
	EC.cursorX = MAX(EC.cursorX, EC.cursorXS);
	if (EC.cursorX > rowLen)
	{
		EC.cursorX = rowLen;
	}

	// snap to the last char before the tab char on vertical line changes
	// new cursorX is at most the old cursorX
	if (direction == ARROW_DOWN || direction == ARROW_UP)
		EC.cursorX = editorRealCursorXOnLineWithTabs(&EC.row[EC.cursorY], EC.cursorX);
}

void editorProcessKeyEvent()
{
	int key = editorReadKey();
	switch (key)
	{
	case CTRL_KEY('q'):
		editorExit();
		break;
	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
		editorMoveCursor(key);
		break;
	case PAGE_UP:
	case PAGE_DOWN:
	{
		/* handle row offset and preserve line position */
		if (key == PAGE_UP)
		{
			EC.rowOffset -= EC.screenRows;
			if (EC.rowOffset < 0)
				EC.rowOffset = 0;
		}
		else if (key == PAGE_DOWN)
		{
			EC.rowOffset += EC.screenRows;
			if (EC.rowOffset > EC.numRows)
				EC.rowOffset = EC.numRows;
		}

		/* update cursor position */
		int shifts = EC.screenRows;
		while (shifts--)
		{
			editorMoveCursor(key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
		}
	}
	break;
	case HOME_KEY:
		EC.cursorX = 0;
		break;
	case END_KEY:
		EC.cursorX = EC.screenColumns - 1;
		break;
	case CTRL_KEY('d'):
	{
		FILE *fp = fopen("log.txt", "a+");
		fprintf(fp, "cursorY: %d, rowOffset: %d\n", EC.cursorY, EC.rowOffset);
		fclose(fp);
	}
	break;
	}
}

/*** output ***/
void editorScroll()
{
	EC.renderX = 0;
	if (EC.cursorY < EC.numRows)
	{
		EC.renderX = editorRowCursorXToRenderX(&EC.row[EC.cursorY], EC.cursorX);
	}

	if (EC.cursorY < EC.rowOffset)
	{
		EC.rowOffset = EC.cursorY;
	}
	if (EC.cursorY >= EC.rowOffset + EC.screenRows)
	{
		EC.rowOffset = EC.cursorY - EC.screenRows + 1;
	}
	if (EC.renderX < EC.columnOffset)
	{
		EC.columnOffset = EC.renderX;
	}
	if (EC.renderX >= EC.columnOffset + EC.screenColumns)
	{
		EC.columnOffset = EC.renderX - EC.screenColumns + 1;
	}
}

void editorDrawWelcomeMessage(struct abuf *ab)
{
	char welcomeMsg[80];
	int msgLen = snprintf(welcomeMsg, sizeof(welcomeMsg),
						  "Mimic Text Editor -- version %s", MTE_VERSION);
	if (msgLen > EC.screenColumns)
		msgLen = EC.screenColumns;

	int padding = (EC.screenColumns - msgLen) / 2;
	if (padding)
	{
		abAppend(ab, "~", 1);
		--padding;
	}

	while (padding--)
		abAppend(ab, " ", 1);

	abAppend(ab, welcomeMsg, msgLen);
}

void editorDrawRows(struct abuf *ab)
{
	for (int y = 0; y < EC.screenRows; ++y)
	{
		int rowIndex = y + EC.rowOffset;
		// empty rows
		if (rowIndex >= EC.numRows)
		{
			if (EC.numRows == 0 && y == EC.screenRows / 3)
			{
				editorDrawWelcomeMessage(ab);
			}
			else
			{
				abAppend(ab, "~", 1);
			}
		}
		else
		{
			int len = EC.row[rowIndex].rsize - EC.columnOffset;
			if (len < 0)
				len = 0;
			if (len > EC.screenColumns)
				len = EC.screenColumns;
			abAppend(ab, &EC.row[rowIndex].render[EC.columnOffset], len);
		}

		// clear line end & append next line
		abAppend(ab, "\x1b[K", 3);
		if (y < EC.screenRows - 1)
			abAppend(ab, "\r\n", 2);
	}
}

void editorRefresh()
{
	editorScroll();

	struct abuf ab = ABUF_INIT;

	// hide cursor while repainting
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", EC.cursorY - EC.rowOffset + 1,
			 EC.renderX - EC.columnOffset + 1);
	abAppend(&ab, buf, strlen(buf));

	// show cursor
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** init ***/
void initEditor()
{
	EC.cursorX = EC.cursorXS = EC.cursorY = 0;
	EC.numRows = 0;
	EC.rowOffset = EC.columnOffset = 0;
	EC.renderX = 0;
	EC.row = NULL;

	if (getWindowSize(&EC.screenRows, &EC.screenColumns) == -1)
		terminate("[Error] getWindowSize");
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if (argc >= 2)
		editorOpen(argv[1]);

	while (1)
	{
		editorRefresh();
		editorProcessKeyEvent();
	}

	return 0;
}