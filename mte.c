/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/param.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define MTE_VERSION "0.0.1"
#define TAB_STOP 8
#define KILO_QUIT_TIMES 2
#define ESC_KEY '\x1b'
#define ESC_SEQ(seq) "\x1b[" seq
#define ENTER_KEY '\r'
#define NEW_LINE "\r\n"
#define SEPARATORS ",.;%<>()[]{}+-*/~="

#define ESC_SEQ_CLEAR_SCREEN "\x1b[2J]"
#define ESC_SEQ_DEFAULT_BG_COLOR "\x1b[m"
#define ESC_SEQ_DEFAULT_FG_COLOR "\x1b[39m"
#define ESC_SEQ_DISABLE_ALT_SCREEN "\x1b[?1049l"
#define ESC_SEQ_ENABLE_ALT_SCREEN "\x1b[?1049h"
#define ESC_SEQ_ERASE_INLINE "\x1b[K"
#define ESC_SEQ_GET_CURSOR "\x1b[6n"
#define ESC_SEQ_HIDE_CURSOR "\x1b[?25l"
#define ESC_SEQ_INVERT_BG_COLOR "\x1b[7m"
#define ESC_SEQ_RESET_CURSOR "\x1b[H"
#define ESC_SEQ_SHOW_CURSOR "\x1b[?25h"

#define NEW_LINE_SZ 2
#define ESC_SEQ_CLEAR_SCREEN_SZ 4
#define ESC_SEQ_DEFAULT_BG_COLOR_SZ 3
#define ESC_SEQ_DEFAULT_FG_COLOR_SZ 5
#define ESC_SEQ_DISABLE_ALT_SCREEN_SZ 8
#define ESC_SEQ_ENABLE_ALT_SCREEN_SZ 8
#define ESC_SEQ_ERASE_INLINE_SZ 3
#define ESC_SEQ_GET_CURSOR_SZ 4
#define ESC_SEQ_HIDE_CURSOR_SZ 6
#define ESC_SEQ_INVERT_BG_COLOR_SZ 4
#define ESC_SEQ_RESET_CURSOR_SZ 3
#define ESC_SEQ_SHOW_CURSOR_SZ 6

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRING (1 << 1)

enum EditorKey
{
	BACKSPACE = 127,
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

enum EditorHighlight
{
	HL_NORMAL = 0,
	HL_NUMBER,
	HL_MATCH,
	HL_STRING,
};

/*** data ***/
struct EditorSyntax
{
	char *fileType;
	char **filematch;
	int flags;
};

typedef struct EditorRow
{
	int size;
	int rsize;
	char *chars;
	char *render;
	unsigned char *highlight;
} EditorRow;

struct EditorContext
{
	int rowOffset, columnOffset;
	int screenRows, screenColumns;
	int cursorX, cursorY, cursorXS;
	int renderX;
	int numRows;
	int messageLifeTime;
	int dirty;
	char *filename;
	char statusMsg[80];
	time_t statusMsgTime;
	EditorRow *row;
	struct EditorSyntax *syntax;
	struct termios oldtio;
} EC;

/*** filetypes ***/
char *C_HL_EXTENSIONS[] = {".c", ".h", ".cpp", ".hpp", NULL};

// store all HL catagories
struct EditorSyntax HLDB[] = {
	{"c",
	 C_HL_EXTENSIONS,
	 HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRING},
};

#define HLDB_ENTRIES (int)(sizeof(HLDB) / sizeof(HLDB[0]))

/*** function prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void throwErrorLog(const char *fmt, ...);
void editorFreeRow(EditorRow *row);
void editorRefresh();
char *editorPrompt(const char *prompt, void (*callback)(char *s, int));
int editorRenderXToCursorX(const EditorRow *row, int cursorX);

/*** terminal ***/
void releaseMemory()
{
	free(EC.filename);
	if (EC.row)
	{
		for (int i = 0; i < EC.numRows; ++i)
		{
			editorFreeRow(&EC.row[i]);
		}
		free(EC.row);
	}
}

void terminate(const char *s)
{
	(void)!write(STDOUT_FILENO, ESC_SEQ_CLEAR_SCREEN, ESC_SEQ_CLEAR_SCREEN_SZ);
	(void)!write(STDOUT_FILENO, ESC_SEQ_RESET_CURSOR, ESC_SEQ_RESET_CURSOR_SZ);
	perror(s);
	releaseMemory();
	exit(1);
}

void disableRawMode()
{
	// Reset raw mode & input leftover will be discarded
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EC.oldtio) == -1)
	{
		terminate("[Error] tcsetattr");
	}
}

void enableRawMode()
{
	// Get current parameters & register exit recovery
	if (tcgetattr(STDIN_FILENO, &EC.oldtio) == -1)
	{
		terminate("[Error] tcgetattr");
	}
	atexit(disableRawMode);

	struct termios newtio = EC.oldtio;
	newtio.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	newtio.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	newtio.c_cflag |= (CS8);
	newtio.c_oflag &= ~(OPOST);
	newtio.c_cc[VMIN] = 0;
	newtio.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newtio) == -1)
	{
		terminate("[Error] tcsetattr");
	}

	(void)!write(STDOUT_FILENO, ESC_SEQ_ENABLE_ALT_SCREEN, ESC_SEQ_ENABLE_ALT_SCREEN_SZ);
}

int editorReadKey()
{
	ssize_t size;
	char c;
	while ((size = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (size == -1 && errno != EAGAIN)
		{
			terminate("[Error] read");
		}
	}

	// ESC keys
	if (c == ESC_KEY)
	{
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1)
		{
			return ESC_KEY;
		}
		if (read(STDIN_FILENO, &seq[1], 1) != 1)
		{
			return ESC_KEY;
		}

		if (seq[0] == '[')
		{
			if (seq[1] >= '0' && seq[1] <= '9')
			{
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					return ESC_KEY;
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
		return ESC_KEY;
	}
	return c;
}

void editorExit()
{
	(void)!write(STDOUT_FILENO, ESC_SEQ_CLEAR_SCREEN, ESC_SEQ_CLEAR_SCREEN_SZ);
	(void)!write(STDOUT_FILENO, ESC_SEQ_RESET_CURSOR, ESC_SEQ_RESET_CURSOR_SZ);
	(void)!write(STDOUT_FILENO, ESC_SEQ_DISABLE_ALT_SCREEN, ESC_SEQ_DISABLE_ALT_SCREEN_SZ);
	releaseMemory();
	exit(0);
}

int getCursorPosition(int *rows, int *cols)
{
	char buf[32];
	unsigned int i = 0;

	// request cursor position
	if (write(STDOUT_FILENO, ESC_SEQ_GET_CURSOR, ESC_SEQ_GET_CURSOR_SZ) != 4)
	{
		return -1;
	}

	// read into buffer
	while (i < sizeof(buf) - 1)
	{
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
		{
			return -1;
		}
		if (buf[i] == 'R')
		{
			break;
		}
		++i;
	}
	buf[i] = '\0';

	// validate
	if (buf[0] != ESC_KEY || buf[1] != '[')
	{
		return -1;
	}

	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
	{
		return -1;
	}

	return 0;
}

int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;

	// ioctl get screen size values into ws on success
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
	{
		// fallbacks
		if (write(STDOUT_FILENO, ESC_SEQ("999C") ESC_SEQ("999B"), 12) != 12)
		{
			return -1;
		}
		return getCursorPosition(rows, cols);
	}

	*rows = ws.ws_row;
	*cols = ws.ws_col;

	return 0;
}

/*** Syntax highlight***/
int isSeparator(int c)
{
	return isspace(c) || strchr(SEPARATORS, c) != NULL;
}

void editorUpdateSyntax(EditorRow *row)
{
	row->highlight = realloc(row->highlight, row->rsize);
	if (!row->highlight)
	{
		terminate("[error] realloc");
	}
	memset(row->highlight, HL_NORMAL, row->rsize);

	if (!EC.syntax)
	{
		return;
	}

	int lastSeparator = 1;
	int inString = 0;

	int i = 0;
	while (i < row->rsize)
	{
		char c = row->render[i];
		unsigned char lastHighlight = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

		if (EC.syntax->flags & HL_HIGHLIGHT_STRING)
		{
			if (inString)
			{
				row->highlight[i] = HL_STRING;

				if (c == '\\' && i + 1 < row->rsize)
				{
					row->highlight[i + 1] = HL_STRING;
					i += 2;
					continue;
				}

				// end of string
				if (c == inString)
				{
					inString = 0;
				}
				i++;
				lastSeparator = 1;
				continue;
			}

			// start of string
			if (c == '"' || c == '\'')
			{
				inString = c;
				row->highlight[i] = HL_STRING;
				i++;
				continue;
				;
			}
		}

		if (EC.syntax->flags & HL_HIGHLIGHT_NUMBERS)
		{
			if ((isdigit(c) && (lastSeparator || lastHighlight == HL_NUMBER)) || (c == '.' && lastHighlight == HL_NUMBER))
			{
				row->highlight[i] = HL_NUMBER;
				i++;
				lastSeparator = 0;
				continue;
			}
		}

		lastSeparator = isSeparator(c);
		i++;
	}
}

int editorSyntaxToColor(int highlight)
{
	switch (highlight)
	{
	case HL_NUMBER:
		return 36;
	case HL_MATCH:
		return 33;
	case HL_STRING:
		return 35;
	default:
		return 37;
	}
}

void editorSelectSyntaxHighlight()
{
	if (EC.filename == NULL)
	{
		EC.syntax = NULL;
		return;
	}

	char *ext = strrchr(EC.filename, '.');
	if (ext == NULL)
	{
		EC.syntax = NULL;
		return;
	}

	// iterate HL catagories
	for (int j = 0; j < HLDB_ENTRIES; j++)
	{
		struct EditorSyntax *syntax = &HLDB[j];

		// check matching specific extension
		for (int i = 0; syntax->filematch[i] != NULL; i++)
		{
			int is_ext = (syntax->filematch[i][0] == '.');

			// check extension matches or contains specific name
			if ((is_ext && strcmp(ext, syntax->filematch[i]) == 0) ||
				(!is_ext && strstr(EC.filename, syntax->filematch[i]) != NULL))
			{
				EC.syntax = syntax;
				// re-apply highlight
				for (int fileRow = 0; fileRow < EC.numRows; ++fileRow)
				{
					editorUpdateSyntax(&EC.row[fileRow]);
				}

				return;
			}
		}
	}

	// if no syntax is found, set syntax to NULL
	EC.syntax = NULL;
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

// updates the rendered representation of a row of text in the editor.
void editorUpdateRow(EditorRow *row)
{
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
	{
		if (row->chars[j] == '\t')
		{
			tabs++;
		}
	}

	free(row->render);
	// each tab is 8 chars so +7 per tab
	row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);
	if (!row->render)
	{
		terminate("[error] malloc");
	}
	int at = 0;
	for (j = 0; j < row->size; j++)
	{
		if (row->chars[j] == '\t')
		{
			row->render[at++] = ' ';
			while (at % TAB_STOP != 0)
			{
				row->render[at++] = ' ';
			}
		}
		else
		{
			row->render[at++] = row->chars[j];
		}
	}
	row->render[at] = '\0';
	row->rsize = at;

	editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len)
{
	if (at < 0 || at > EC.numRows)
	{
		return;
	}
	EC.row = realloc(EC.row, sizeof(EditorRow) * (EC.numRows + 1));
	if (!EC.row)
	{
		terminate("[error] realloc");
	}
	memmove(&EC.row[at + 1], &EC.row[at], sizeof(EditorRow) * (EC.numRows - at));

	EditorRow *newRow = &EC.row[at];
	newRow->chars = malloc(len + 1);
	if (!newRow->chars)
	{
		terminate("[error] malloc");
	}

	// strcpy stops at null byte, use memcpy instead
	memcpy(newRow->chars, s, len);
	newRow->chars[len] = '\0';
	newRow->size = len;
	newRow->rsize = 0;
	newRow->render = NULL;
	newRow->highlight = NULL;
	editorUpdateRow(newRow);

	EC.dirty++;
	EC.numRows++;
}

void editorFreeRow(EditorRow *row)
{
	free(row->render);
	free(row->chars);
	free(row->highlight);
}

void editorDelRow(int at)
{
	if (at < 0 || at >= EC.numRows)
	{
		return;
	}
	editorFreeRow(&EC.row[at]);
	memmove(&EC.row[at], &EC.row[at + 1], sizeof(EditorRow) * (EC.numRows - at - 1));
	EC.numRows--;
	EC.dirty++;
}

void editorRowAppendString(EditorRow *row, const char *s, size_t len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(row->chars + row->size, s, len);

	row->size += len;
	row->chars[row->size] = '\0';

	editorUpdateRow(row);
	EC.dirty++;
}

void editorRowInsertChar(EditorRow *row, int at, int c)
{
	if (at < 0 || at > row->size)
	{
		at = row->size;
	}
	// 1 byte for new char, 1 more for null byte
	char *newPtr = realloc(row->chars, row->size + 2);
	if (!newPtr)
	{
		terminate("[error] realloc");
	}
	row->chars = newPtr;
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editorUpdateRow(row);
	EC.dirty++;
}

void editorInsertNewline()
{
	if (EC.cursorX == 0)
	{
		editorInsertRow(EC.cursorY, "", 0);
	}
	else
	{
		EditorRow *currentRow = &EC.row[EC.cursorY];
		size_t newRowLength = currentRow->size - EC.cursorX;
		editorInsertRow(EC.cursorY + 1, &currentRow->chars[EC.cursorX], newRowLength);

		currentRow = &EC.row[EC.cursorY];
		currentRow->size = EC.cursorX;
		currentRow->chars[currentRow->size] = '\0';

		editorUpdateRow(currentRow);
	}
	EC.cursorY++;
	EC.cursorX = EC.cursorXS = 0;
}

void editorRowDelChar(EditorRow *row, int at)
{
	if (at < 0 || at >= row->size)
	{
		return;
	}
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editorUpdateRow(row);
	EC.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c)
{
	if (EC.cursorY == EC.numRows)
	{
		editorInsertRow(EC.numRows, "", 0);
	}
	editorRowInsertChar(&EC.row[EC.cursorY], EC.cursorX, c);
	EC.cursorXS = ++EC.cursorX;
}

void editorDelChar()
{
	if (EC.cursorY == EC.numRows)
	{
		return;
	}

	EditorRow *currentRow = &EC.row[EC.cursorY];

	if (EC.cursorX > 0)
	{
		editorRowDelChar(currentRow, EC.cursorX - 1);
		EC.cursorX--;
		EC.cursorXS = editorRowCursorXToRenderX(currentRow, EC.cursorX);
		return;
	}

	if (EC.cursorY == 0)
	{
		return;
	}

	EC.cursorX = EC.row[EC.cursorY - 1].size;
	editorRowAppendString(&EC.row[EC.cursorY - 1], currentRow->chars, currentRow->size);
	editorDelRow(EC.cursorY);
	EC.cursorY--;
	EC.cursorXS = editorRowCursorXToRenderX(&EC.row[EC.cursorY], EC.cursorX);
}

/*** file IO ***/
char *editorRowsToString(int *buflen)
{
	int totalLength = 0;

	for (int i = 0; i < EC.numRows; i++)
	{
		totalLength += EC.row[i].size + 1;
	}

	char *buffer = malloc(totalLength + 1); // Add one for the null-terminator
	char *p = buffer;
	for (int i = 0; i < EC.numRows; i++)
	{
		memcpy(p, EC.row[i].chars, EC.row[i].size);
		p += EC.row[i].size;
		*p++ = '\n';
	}
	*p = '\0';

	*buflen = totalLength;

	return buffer;
}

void editorOpen(const char *filename)
{
	free(EC.filename);
	size_t fnlen = strlen(filename) + 1;
	EC.filename = malloc(fnlen);
	if (EC.filename)
	{
		memcpy(EC.filename, filename, fnlen);
	}

	editorSelectSyntaxHighlight();

	FILE *fp = fopen(filename, "r");
	if (!fp)
	{
		terminate("[Error] fopen");
	}

	char *line = NULL;
	ssize_t lineLen = 0;
	size_t size = 0;
	while ((lineLen = getline(&line, &size, fp)) != -1)
	{
		// strip next line char
		while (lineLen > 0 && (line[lineLen - 1] == '\n' ||
							   line[lineLen - 1] == ENTER_KEY))
		{
			lineLen--;
		}
		editorInsertRow(EC.numRows, line, lineLen);
	}
	EC.dirty = 0;
	free(line);
	fclose(fp);
}

int editorSave()
{
	if (!EC.filename)
	{
		EC.filename = editorPrompt("Save as: %s", NULL);
		if (!EC.filename)
		{
			editorSetStatusMessage("Cancelled");
			return 1;
		}

		// update syntax after naming
		editorSelectSyntaxHighlight();
	}

	const char *extension = ".tmp";
	char tmpFilename[128];
	strcpy(tmpFilename, EC.filename);
	strncat(tmpFilename, extension, strlen(extension));

	int bufferLength;
	char *buf = editorRowsToString(&bufferLength);

	int fd = open(tmpFilename, O_RDWR | O_CREAT, 0644);
	if (fd == -1)
	{
		goto writeerr;
	}

	if (ftruncate(fd, bufferLength) == -1)
	{
		goto writeerr;
	}

	if (write(fd, buf, bufferLength) == -1)
	{
		goto writeerr;
	}

	close(fd);
	free(buf);
	rename(tmpFilename, EC.filename);
	EC.dirty = 0;
	editorSetStatusMessage("%d bytes written to disk (%s)", bufferLength, EC.filename);
	return 0;

writeerr:
	free(buf);
	if (fd != -1)
	{
		close(fd);
	}
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
	return 1;
}

/*** search ***/
void editorSearchCallback(char *pattern, int key)
{
	static int lastMatchRow = -1;
	static int lastMatchX = -1;
	static int direction = 1;
	static int savedHighlightLine;
	static char *savedHighlightChars = NULL;

	// restore highlight
	if (savedHighlightChars)
	{
		memcpy(EC.row[savedHighlightLine].highlight, savedHighlightChars, EC.row[savedHighlightLine].rsize);
		free(savedHighlightChars);
		savedHighlightChars = NULL;
	}

	if (key == ENTER_KEY || key == ESC_KEY)
	{
		lastMatchRow = -1;
		lastMatchX = -1;
		direction = 1;
		return;
	}

	switch (key)
	{
	case ARROW_UP:
	case ARROW_LEFT:
		direction = -1;
		break;
	case ARROW_RIGHT:
	case ARROW_DOWN:
		direction = 1;
		break;
	default:
		lastMatchRow = -1;
		lastMatchX = -1;
		direction = 1;
		break;
	}

	int currentLine = MAX(lastMatchRow, 0);
	int currentX = lastMatchX;
	for (int i = 0; i < EC.numRows; ++i)
	{
		if (currentLine < 0)
		{
			currentLine = EC.numRows - 1;
		}
		else if (currentLine == EC.numRows)
		{
			currentLine = 0;
		}

		const EditorRow *row = &EC.row[currentLine];
		char *match = NULL;
		if (direction > 0)
		{
			if (currentX == -1)
				match = strstr(row->render, pattern);
			else
				match = strstr(row->render + currentX + 1, pattern);
		}
		else
		{
			char *lastPossibleMatch = NULL;
			char *searchStart = row->render;
			while ((match = strstr(searchStart, pattern)) && (currentX == -1 || (match < row->render + currentX)))
			{
				lastPossibleMatch = match;
				searchStart = match + 1;
			}
			match = lastPossibleMatch;
		}

		if (match)
		{
			lastMatchRow = currentLine;
			EC.cursorY = currentLine;
			EC.rowOffset = currentLine;
			lastMatchX = match - row->render;
			EC.cursorX = editorRenderXToCursorX(row, match - row->render + strlen(pattern));
			EC.cursorXS = EC.cursorX;

			// Save for highlight restore
			savedHighlightLine = currentLine;
			savedHighlightChars = malloc(row->rsize);
			memcpy(savedHighlightChars, row->highlight, row->rsize);
			memset(&row->highlight[match - row->render], HL_MATCH, strlen(pattern));
			break;
		}

		lastMatchX = -1;
		currentX = -1;
		currentLine += direction;
	}
}

void editorSearch()
{
	int originCursorX = EC.cursorX, originCursorY = EC.cursorY, originCursorXS = EC.cursorXS;
	int originRowOffset = EC.rowOffset, originColumnOffset = EC.columnOffset;

	char *pattern = editorPrompt("Search: %s (Press ESC or Ctrl+C to cancel)", editorSearchCallback);
	if (pattern)
	{
		free(pattern);
		return;
	}

	EC.cursorX = originCursorX;
	EC.cursorXS = originCursorXS;
	EC.cursorY = originCursorY;
	EC.rowOffset = originRowOffset;
	EC.columnOffset = originColumnOffset;
}

/*** buffer append ***/
struct abuf
{
	char *b;
	int len;
};

#define ABUF_INIT \
	{             \
		NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
	char *newBuf = realloc(ab->b, ab->len + len);
	if (newBuf)
	{
		memcpy(&newBuf[ab->len], s, len);
		ab->b = newBuf;
		ab->len += len;
	}
}

void abFree(struct abuf *ab)
{
	free(ab->b);
}

/*** input ***/

// return the buffer entered by the user. Returned buffer needs to be manually free after use.
char *editorPrompt(const char *prompt, void (*callback)(char *s, int))
{
	size_t bufsize = 128;
	char *buffer = malloc(bufsize);
	if (!buffer)
	{
		terminate("[error] malloc");
	}

	size_t buflen = 0;
	buffer[0] = '\0';

	while (1)
	{
		editorSetStatusMessage(prompt, buffer);
		editorRefresh();

		int c = editorReadKey();
		if (c == ESC_KEY || c == CTRL_KEY('c'))
		{
			editorSetStatusMessage("");
			if (callback)
			{
				callback(buffer, c);
			}
			free(buffer);
			return NULL;
		}

		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
		{
			if (buflen)
			{
				buffer[--buflen] = '\0';
			}
		}
		else if (c == ENTER_KEY)
		{
			if (buflen != 0)
			{
				editorSetStatusMessage("");
				if (callback)
				{
					callback(buffer, c);
				}
				return buffer;
			}
		}
		else if (!iscntrl(c) && c < 128)
		{
			if (buflen == bufsize - 1)
			{
				bufsize *= 2;
				buffer = realloc(buffer, bufsize);
				if (!buffer)
				{
					terminate("[error] realloc");
				}
			}
			buffer[buflen++] = c;
			buffer[buflen] = '\0';
		}

		if (callback)
		{
			callback(buffer, c);
		}
	}
}

int editorRenderXToCursorX(const EditorRow *row, int cursorX)
{
	int realCursorX = 0;

	for (int i = 0; i < cursorX; i++)
	{
		if (row->chars[i] == '\t')
		{
			realCursorX += (TAB_STOP - (realCursorX % TAB_STOP));
		}
		else
		{
			realCursorX++;
		}

		if (realCursorX > cursorX)
		{
			return i;
		}
	}

	return realCursorX;
}

void editorMoveCursorLeft()
{
	if (EC.cursorX > 0)
	{
		EC.cursorX--;
		EC.cursorXS = editorRowCursorXToRenderX(&EC.row[EC.cursorY], EC.cursorX);
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
	if (EC.cursorY >= EC.numRows)
	{
		return;
	}

	if (EC.cursorX < EC.row[EC.cursorY].size)
	{
		EC.cursorX++;
		EC.cursorXS = editorRowCursorXToRenderX(&EC.row[EC.cursorY], EC.cursorX);
		return;
	}

	EC.cursorY++;
	EC.cursorX = 0;
	EC.cursorXS = EC.cursorX;
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

void editorRefreshCursor()
{
	int rowLen;
	if (EC.cursorY < EC.numRows)
	{
		rowLen = EC.row[EC.cursorY].size;
	}
	else
	{
		EC.cursorY = EC.numRows;
		rowLen = 0;
	}

	if (EC.cursorX < 0)
	{
		EC.cursorX = 0;
	}
	else if (EC.cursorX > rowLen)
	{
		EC.cursorX = rowLen;
	}

	EC.cursorXS = EC.cursorX;
	EC.cursorX = editorRenderXToCursorX(&EC.row[EC.cursorY], EC.cursorX);
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

	// snap to the last char before the tab char on vertical line changes
	if (direction == ARROW_DOWN || direction == ARROW_UP)
	{
		// back to the last cursor X before the snapping
		EC.cursorX = EC.cursorXS;
		// new cursorX is at most the old cursorX
		EC.cursorX = editorRenderXToCursorX(&EC.row[EC.cursorY], EC.cursorX);
	}

	int rowLen = (EC.cursorY >= EC.numRows) ? 0 : EC.row[EC.cursorY].size;
	if (EC.cursorX > rowLen)
	{
		EC.cursorX = rowLen;
	}
}

void editorProcessKeyEvent()
{
	static int quitTimes = KILO_QUIT_TIMES;

	int key = editorReadKey();
	switch (key)
	{
	case CTRL_KEY('q'):
	{
		if (EC.dirty && quitTimes > 0)
		{
			editorSetStatusMessage("Discard unsaved buffer? Press "
								   "Ctrl-Q %d more times to quit.",
								   quitTimes);
			quitTimes--;
			return;
		}
		editorExit();
	}
	break;
	case CTRL_KEY('s'):
	{
		editorSave();
	}
	break;
	case ARROW_UP:
	case ARROW_DOWN:
	case ARROW_LEFT:
	case ARROW_RIGHT:
	{
		editorMoveCursor(key);
	}
	break;
	case PAGE_UP:
	case PAGE_DOWN:
	{
		/* handle row offset and preserve line position */
		if (key == PAGE_UP)
		{
			EC.rowOffset -= EC.screenRows;
			if (EC.rowOffset < 0)
			{
				EC.rowOffset = 0;
			}
		}
		else if (key == PAGE_DOWN)
		{
			EC.rowOffset += EC.screenRows;
			if (EC.rowOffset > EC.numRows)
			{
				EC.rowOffset = EC.numRows;
			}
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
	{
		EC.cursorX = 0;
		EC.cursorXS = EC.cursorX;
	}
	break;
	case END_KEY:
	{
		EC.cursorX = EC.row[EC.cursorY].size;
		EC.cursorXS = EC.cursorX;
	}
	break;
	case CTRL_KEY('d'):
	{
		FILE *fp = fopen("log.txt", "a+");
		fprintf(fp, "cursorX: %d, cursorXS: %d, renderX: %d, renderX: %d\n", EC.cursorX, EC.cursorXS, EC.renderX, editorRowCursorXToRenderX(&EC.row[EC.cursorY], EC.cursorX));
		fclose(fp);
	}
	break;
	case ENTER_KEY:
		editorInsertNewline();
		break;
	case BACKSPACE:
	case CTRL_KEY('h'):
	case DEL_KEY:
	{
		if (key == DEL_KEY)
		{
			editorMoveCursor(ARROW_RIGHT);
		}
		editorDelChar();
	}
	break;
	case CTRL_KEY('l'):
	case ESC_KEY:
		/* unimplemented */
		break;
	case CTRL_KEY('f'):
		editorSearch();
		break;
	default:
		editorInsertChar(key);
		break;
	}
	quitTimes = KILO_QUIT_TIMES;
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

void editorDrawMessageBar(struct abuf *ab)
{
	abAppend(ab, ESC_SEQ_ERASE_INLINE, ESC_SEQ_ERASE_INLINE_SZ);
	int msgLen = strlen(EC.statusMsg);
	if (msgLen > EC.screenColumns)
	{
		msgLen = EC.screenColumns;
	}
	if (msgLen && time(NULL) - EC.statusMsgTime < EC.messageLifeTime)
	{
		abAppend(ab, EC.statusMsg, msgLen);
	}
}

void editorDrawStatusBar(struct abuf *ab)
{
	// switch to inverted color
	abAppend(ab, ESC_SEQ_INVERT_BG_COLOR, ESC_SEQ_INVERT_BG_COLOR_SZ);

	char status[80], rstatus[80];
	int statusLen = snprintf(status, sizeof(status), "%.20s - %d lines %s",
							 EC.filename ? EC.filename : "[Unamed]", EC.numRows, EC.dirty ? "(modified)" : "");
	int rstatusLen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", EC.syntax ? EC.syntax->fileType : "No filetype", EC.cursorY + 1, EC.numRows);
	if (statusLen > EC.screenColumns)
	{
		statusLen = EC.screenColumns;
	}
	abAppend(ab, status, statusLen);

	for (int i = 0; i < EC.screenColumns - statusLen - rstatusLen; ++i)
	{
		abAppend(ab, " ", 1);
	}
	abAppend(ab, rstatus, rstatusLen);

	// switch back to normal color
	abAppend(ab, ESC_SEQ_DEFAULT_BG_COLOR, ESC_SEQ_DEFAULT_BG_COLOR_SZ);
	abAppend(ab, NEW_LINE, NEW_LINE_SZ);
}

void editorDrawWelcomeMessage(struct abuf *ab)
{
	char welcomeMsg[80];
	int msgLen = snprintf(welcomeMsg, sizeof(welcomeMsg),
						  "Mimic Text Editor -- version %s", MTE_VERSION);
	if (msgLen > EC.screenColumns)
	{
		msgLen = EC.screenColumns;
	}

	int padding = (EC.screenColumns - msgLen) / 2;
	if (padding)
	{
		abAppend(ab, "~", 1);
		--padding;
	}

	while (padding--)
	{
		abAppend(ab, " ", 1);
	}

	abAppend(ab, welcomeMsg, msgLen);
}

void editorDrawRows(struct abuf *ab)
{
	for (int y = 0; y < EC.screenRows; ++y)
	{
		int rowIndex = y + EC.rowOffset;

		if (rowIndex >= EC.numRows)
		{
			// Display welcome message or tilde on empty rows
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
			// Determine the number of characters to draw from the current row
			int len = EC.row[rowIndex].rsize - EC.columnOffset;
			if (len < 0)
			{
				len = 0;
			}
			if (len > EC.screenColumns)
			{
				len = EC.screenColumns;
			}

			char *c = &EC.row[rowIndex].render[EC.columnOffset];
			unsigned char *hl = &EC.row[rowIndex].highlight[EC.columnOffset];
			int currentColor = -1;
			// Draw the visible portion of the row
			for (int j = 0; j < len; j++)
			{
				// Keep normal text in default. Highlight digits in Red
				if (hl[j] == HL_NORMAL)
				{
					if (currentColor != HL_NORMAL)
					{
						abAppend(ab, ESC_SEQ_DEFAULT_FG_COLOR, ESC_SEQ_DEFAULT_FG_COLOR_SZ);
						currentColor = HL_NORMAL;
					}
					abAppend(ab, &c[j], 1);
				}
				else
				{
					int color = editorSyntaxToColor(hl[j]);
					if (color != currentColor)
					{
						currentColor = color;
						char buf[16];
						int clen = snprintf(buf, sizeof(buf), ESC_SEQ("%dm"), color);
						abAppend(ab, buf, clen);
					}
					abAppend(ab, &c[j], 1);
				}
			}
			abAppend(ab, ESC_SEQ_DEFAULT_FG_COLOR, ESC_SEQ_DEFAULT_FG_COLOR_SZ);
		}

		// Clear the line from the cursor to the end and move to the next line
		abAppend(ab, ESC_SEQ_ERASE_INLINE, ESC_SEQ_ERASE_INLINE_SZ);
		abAppend(ab, NEW_LINE, NEW_LINE_SZ);
	}
}

// variadic function
void editorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(EC.statusMsg, sizeof(EC.statusMsg), fmt, ap);
	va_end(ap);
	EC.statusMsgTime = time(NULL);
}

void throwErrorLog(const char *fmt, ...)
{

	FILE *fp = fopen("error.log", "a+");
	if (fp)
	{
		va_list ap;
		va_start(ap, fmt);
		fprintf(fp, fmt, ap);
		va_end(ap);
		fclose(fp);
	}
}

void editorRefresh()
{
	editorScroll();

	struct abuf ab = ABUF_INIT;

	// hide cursor while repainting
	abAppend(&ab, ESC_SEQ_HIDE_CURSOR, ESC_SEQ_HIDE_CURSOR_SZ);
	abAppend(&ab, ESC_SEQ_RESET_CURSOR, ESC_SEQ_RESET_CURSOR_SZ);

	// draw file rows
	editorDrawRows(&ab);

	// draw status bar
	editorDrawStatusBar(&ab);

	// draw message bar
	editorDrawMessageBar(&ab);

	// draw cursor
	char buf[32];
	snprintf(buf, sizeof(buf), ESC_SEQ("%d;%dH"), EC.cursorY - EC.rowOffset + 1,
			 EC.renderX - EC.columnOffset + 1);
	abAppend(&ab, buf, strlen(buf));

	// show cursor
	abAppend(&ab, ESC_SEQ_SHOW_CURSOR, ESC_SEQ_SHOW_CURSOR_SZ);

	// render
	(void)!write(STDOUT_FILENO, ab.b, ab.len);
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
	EC.filename = NULL;
	EC.statusMsg[0] = '\0';
	EC.statusMsgTime = 0;
	EC.messageLifeTime = 5;
	EC.dirty = 0;
	EC.syntax = NULL;

	if (getWindowSize(&EC.screenRows, &EC.screenColumns) == -1)
	{
		terminate("[Error] getWindowSize");
	}

	// reserve line for status menu
	EC.screenRows -= 2;
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();
	if (argc >= 2)
	{
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("KEY: Ctrl-Q = quit | Ctrl-S = save | Ctrl-F = search");

	while (1)
	{
		editorRefresh();
		editorProcessKeyEvent();
	}

	return 0;
}