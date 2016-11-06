/*
 * This file is part of hx - a hex editor for the terminal.
 *
 * Copyright (c) 2016 Kevin Pors. See LICENSE for details.
 */

// Define _POSIX_SOURCE to enable sigaction(). See `man 2 sigaction'
#define _POSIX_SOURCE

#include "charbuf.h"

// C99 includes
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// POSIX and Linux cruft
#include <getopt.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

// hx defines. Not declared as const because we may want to adjust
// this by using a tool or whatever.

#ifndef HX_GIT_HASH
#define HX_GIT_HASH "unknown"
#endif

#ifndef HX_VERSION
#define HX_VERSION "1.0.0"
#endif

// Declarations.

/**
 * Mode the editor can be in.
 */
enum editor_mode {
	MODE_NORMAL,  // normal mode i.e. for navigating, commands.
	MODE_INSERT,  // insert values at cursor position.
	MODE_REPLACE, // replace values at cursor position.
	MODE_COMMAND, // command input mode.
};

/**
 * Current status severity.
 */
enum status_severity {
	STATUS_INFO,    // appear as lightgray bg, black fg
	STATUS_WARNING, // appear as yellow bg, black fg
	STATUS_ERROR,   // appear as red bg, white fg
};

/**
 * This struct contains internal information of the state of the editor.
 */
struct editor {
	int octets_per_line; // Amount of octets (bytes) per line. Ideally multiple of 2.
	int grouping;        // Amount of bytes per group. Ideally multiple of 2.

	int hex_line_width;  // the width in chars of a hex line, including
	                     // grouping spaces.

	int line;        // The 'line' in the editor. Used for scrolling.
	int cursor_x;    // Cursor x pos on the current screen
	int cursor_y;    // Cursor y pos on the current screen
	int screen_rows; // amount of screen rows after init
	int screen_cols; // amount of screen columns after init

	enum editor_mode mode;           // mode the editor is in
	bool             dirty;          // whether the buffer is modified
	char*            filename;       // the filename currently open
	char*            contents;       // the file's contents
	int              content_length; // length of the contents

	enum status_severity status_severity;                   // status severity
	char                 status_message[120]; // status message

};

// Utility functions.
void enable_raw_mode();
void disable_raw_mode();
void clear_screen();
int  read_key();
bool ishex(const char c);
int  hex2bin(const char* s);
int str2int(const char* s, int min, int max, int def);
void print_version();
void print_help(const char* explanation);

// editor functions:
struct editor* editor_init();

void editor_cursor_at_offset(struct editor* e, int offset, int* x, int *y);
void editor_delete_char_at_cursor(struct editor* e);
void editor_exit();
void editor_free(struct editor* e);
void editor_increment_byte(struct editor* e, int amount);
void editor_move_cursor(struct editor* e, int dir, int amount);
int  editor_offset_at_cursor(struct editor* e);
void editor_openfile(struct editor* e, const char* filename);
void editor_process_keypress(struct editor* e);
void editor_render_ascii(struct editor* e, int rownum, const char* ascii, struct charbuf* b);
void editor_render_contents(struct editor* e, struct charbuf* b);
void editor_render_ruler(struct editor* e, struct charbuf* buf);
void editor_refresh_screen(struct editor* e);
void editor_replace_byte(struct editor* e, char x);
void editor_scroll(struct editor* e, int units);
void editor_setmode(struct editor *e, enum editor_mode mode);
int  editor_statusmessage(struct editor* e, enum status_severity s, const char* fmt, ...);
void editor_writefile(struct editor* e);

// Global editor config.
struct editor* g_ec;

// Terminal IO settings. Used to reset it when exiting to prevent terminal
// data garbling. Declared global since we require it in the atexit() call.
struct termios orig_termios;

// Key enumerations
enum key_codes {
	KEY_NULL     = 0,
	KEY_CTRL_Q   = 0x11, // DC1, to exit the program.
	KEY_CTRL_S   = 0x13, // DC2, to save the current buffer.
	KEY_ESC      = 0x1b, // ESC, for things like keys up, down, left, right, delete, ...

	// 'Virtual keys', i.e. not corresponding to terminal escape sequences
	// or any other ANSI stuff. Merely to identify keys returned by read_key().
	KEY_UP      = 1000, // [A
	KEY_DOWN,           // [B
	KEY_RIGHT,          // [C
	KEY_LEFT,           // [D
	KEY_HOME,           // [H
	KEY_END,            // [F
	KEY_PAGEUP,         // ??
	KEY_PAGEDOWN,       // ??
};

/* ==============================================================================
 * Utility functions.
 * ============================================================================*/

bool ishex(const char c) {
	return
		(c >= '0' && c <= '9') ||
		(c >= 'A' && c <= 'F') ||
		(c >= 'a' && c <= 'f');
}

int hex2bin(const char* s) {
	int ret=0;
	for(int i = 0; i < 2; i++) {
		char c = *s++;
		int n=0;
		if( '0' <= c && c <= '9')  {
			n = c-'0';
		} else if ('a' <= c && c <= 'f') {
			n = 10 + c - 'a';
		} else if ('A' <= c && c <= 'F') {
			n = 10 + c - 'A';
		}
		ret = n + ret*16;
	}
	return ret;
}

/**
 * Parses a string to an integer and returns it. In case of errors, the default
 * `def' will be returned. When the `min' > parsed_value > `max', then the
 * default `def' will also be returned.
 *
 * XXX: probably return some error indicator instead, and exit with a message?
 */
int str2int(const char* s, int min, int max, int def) {
	char* endptr;
	uintmax_t x = strtoimax(s, &endptr, 10);
	if (errno  == ERANGE) {
		return def;
	}
	if (x < min || x > max) {
		return def;
	}
	return x;
}

/**
 * Prints help to the stderr when invoked with -h or with unknown arguments.
 * Explanation can be given for some extra information.
 */
void print_help(const char* explanation) {
	fprintf(stderr,
"%s"\
"usage: hx [-hv] [-o octets_per_line] [-g grouping_bytes] filename\n"\
"\n"
"Command options:\n"
"    -h     Print this cruft and exits\n"
"    -v     Version information\n"
"    -o     Amount of octets per line\n"
"    -g     Grouping of bytes in one line\n"
"\n"
"Currently, both these values are advised to be a multiple of 2\n"
"to prevent garbled display :)\n"
"\n"
"Report bugs to <krpors at gmail.com> or see <http://github.com/krpors/hx>\n"
, explanation);
}

/**
 * Prints some version information back to the stdout.
 */
void print_version() {
	printf("hx version %s (git: %s)\n", HX_VERSION, HX_GIT_HASH);
}

/**
 * Reads keypresses from stdin, and processes them accordingly. Escape sequences
 * will be read properly as well (e.g. DEL will be the bytes 0x1b, 0x5b, 0x33, 0x7e).
 * The returned integer will contain either one of the enum values, or the key pressed.
 *
 * read_key() will only return the correct key code, or -1 when anything fails.
 */
int read_key() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
	if (nread == -1) {
		// This error may happen when a SIGWINCH is received by resizing the terminal.
		// The read() call is interrupted and will fail here. In that case, just return
		// -1 prematurely and continue the main loop. In all other cases, this will
		// be unexpected so inform the user that something has happened.
		if (errno == EINTR) {
			return -1;
		}

		fprintf(stderr, "Unable to read from stdin: %s\n", strerror(errno));
		exit(2);
	}

	char seq[4]; // escape sequence buffer.

	switch (c) {
	case KEY_ESC:
		// Escape key was pressed, OR things like delete, arrow keys, ...
		// So we will try to read ahead a few bytes, and see if there's more.
		// For instance, a single Escape key only produces a single 0x1b char.
		// A delete key produces 0x1b 0x5b 0x33 0x7e.
		if (read(STDIN_FILENO, seq, 1) == 0) {
			return KEY_ESC;
		}
		if (read(STDIN_FILENO, seq + 1, 1) == 0) {
			return KEY_ESC;
		}

		// home = 0x1b, [ = 0x5b, 1 = 0x31, ~ = 0x7e,
		// end  = 0x1b, [ = 0x5b, 4 = 0x34, ~ = 0x7e,
		// pageup   1b, [=5b, 5=35, ~=7e,
		// pagedown 1b, [=5b, 6=36, ~=7e,

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, seq + 2, 1) == 0) {
					return KEY_ESC;
				}
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1': return KEY_HOME;
					case '4': return KEY_END;
					case '5': return KEY_PAGEUP;
					case '6': return KEY_PAGEDOWN;
					}
				}
			}
			switch (seq[1]) {
			case 'A': return KEY_UP;
			case 'B': return KEY_DOWN;
			case 'C': return KEY_RIGHT;
			case 'D': return KEY_LEFT;
			case 'H': return KEY_HOME; // does not work with me?
			case 'F': return KEY_END;  // ... same?
			}
		}
		break;
	}

	return c;
}

bool get_window_size(int* rows, int* cols) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
		perror("Failed to query terminal size");
		exit(1);
	}

	*rows = ws.ws_row;
	*cols = ws.ws_col;
	return true;
}

void enable_raw_mode() {
	// only enable raw mode when stdin is a tty.
	if (!isatty(STDIN_FILENO)) {
		perror("Input is not a TTY");
		exit(1);
	}

	// Disable raw mode when we exit hx normally.
	atexit(editor_exit);

	tcgetattr(STDIN_FILENO, &orig_termios);

	struct termios raw = orig_termios;
	// input modes: no break, no CR to NL, no parity check, no strip char,
	// no start/stop output control.
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	// output modes - disable post processing
	raw.c_oflag &= ~(OPOST);
	// control modes - set 8 bit chars
	raw.c_cflag |= (CS8);
	// local modes - choing off, canonical off, no extended functions,
	// no signal chars (^Z,^C)
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	// control chars - set return condition: min number of bytes and timer.
	// Return each byte, or zero for timeout.
	raw.c_cc[VMIN] = 0;
	// 100 ms timeout (unit is tens of second). Do not set this to 0 for
	// whatever reason, because this will skyrocket the cpu usage to 100%!
	raw.c_cc[VTIME] = 1;

    // put terminal in raw mode after flushing
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
		perror("Unable to set terminal to raw mode");
		exit(1);
	}
}

void disable_raw_mode() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}


void clear_screen() {
	// clear the colors, move the cursor up-left, clear the screen.
	char stuff[80];
	int bw = snprintf(stuff, 80, "\x1b[0m\x1b[H\x1b[2J");
	if (write(STDOUT_FILENO, stuff, bw) == -1) {
		perror("Unable to clear screen");
	}
}

/* ==============================================================================
 * Functions operating on the editor.
 * ============================================================================*/

/**
 * Moves the cursor. The terminal cursor positions are all 1-based, so we
 * take that into account. When we scroll past boundaries (left, right, up
 * and down) we react accordingly. Note that the cursor_x/y are also 1-based,
 * and we calculate the actual position of the hex values by incrementing it
 * later on with the address size, amount of grouping spaces etc.
 *
 * This function looks convoluted as hell, but it works...
 */
void editor_move_cursor(struct editor* e, int dir, int amount) {
	switch (dir) {
	case KEY_UP:    e->cursor_y-=amount; break;
	case KEY_DOWN:  e->cursor_y+=amount; break;
	case KEY_LEFT:  e->cursor_x-=amount; break;
	case KEY_RIGHT: e->cursor_x+=amount; break;
	}
	// Did we hit the start of the file? If so, stop moving and place
	// the cursor on the top-left of the hex display.
	if (e->cursor_x <= 1 && e->cursor_y <= 1 && e->line <= 0) {
		e->cursor_x = 1;
		e->cursor_y = 1;
		return;
	}

	// Move the cursor over the x (columns) axis.
	if (e->cursor_x < 1) {
		// Are we trying to move left on the leftmost boundary?
		//
		// 000000000: 4d49 5420 4c69 6365 6e73 650a 0a43 6f70  MIT License..Cop
		// 000000010: 7972 6967 6874 2028 6329 2032 3031 3620  yright (c) 2016
		//            <--- [cursor goes to the left]
		//
		// Then we go up a row, cursor to the right. Like a text editor.
		if (e->cursor_y >= 1) {
			e->cursor_y--;
			e->cursor_x = e->octets_per_line;
		}
	} else if (e->cursor_x > e->octets_per_line) {
		// Moving to the rightmost boundary?
		//
		// 000000000: 4d49 5420 4c69 6365 6e73 650a 0a43 6f70  MIT License..Cop
		// 000000010: 7972 6967 6874 2028 6329 2032 3031 3620  yright (c) 2016
		//                    [cursor goes to the right] --->
		//
		// Then move a line down, position the cursor to the beginning of the row.
		// Unless it's the end of file.
		e->cursor_y++;
		e->cursor_x = 1;
	}

	// Did we try to move up when there's nothing? For example
	//
	//                       [up here]
	// --------------------------^
	// 000000000: 4d49 5420 4c69 6365 6e73 650a 0a43 6f70  MIT License..Cop
	//
	// Then stop moving upwards, do not scroll, return.
	if (e->cursor_y <= 1 && e->line <= 0) {
		e->cursor_y = 1;
	}

	// Move the cursor over the y axis
	if (e->cursor_y > e->screen_rows - 1) {
		e->cursor_y = e->screen_rows - 1;
		editor_scroll(e, 1);
	} else if (e->cursor_y < 1 && e->line > 0) {
		e->cursor_y = 1;
		editor_scroll(e, -1);
	}

	// Did we hit the end of the file somehow? Set the cursor position
	// to the maximum cursor position possible.
	int offset = editor_offset_at_cursor(e);
	if (offset >= e->content_length - 1) {
		editor_cursor_at_offset(e, offset, &e->cursor_x, &e->cursor_y);
		return;
	}
}
/**
 * Opens a file denoted by `filename', or exit if the file cannot be opened.
 * The editor struct is used to contain the contents and other metadata
 * about the file being opened.
 */

void editor_openfile(struct editor* e, const char* filename) {
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL) {
		fprintf(stderr, "Cannot open file '%s': %s\n", filename, strerror(errno));
		exit(1);
	}

	// stat() the file.
	struct stat statbuf;
	if (stat(filename, &statbuf) == -1) {
		perror("Cannot stat file");
		exit(1);
	}
	// S_ISREG is a a POSIX macro to check whether the given st_mode denotes a
	// regular file. See `man 2 stat'.
	if (!S_ISREG(statbuf.st_mode)) {
		fprintf(stderr, "File '%s' is not a regular file\n", filename);
		exit(1);
	}

	// go to the end of the file.
	fseek(fp, 0, SEEK_END);
	// determine file size
	long size = ftell(fp);
	// set the indicator to the start of the file
	fseek(fp, 0, SEEK_SET);

	if (size <= 0) {
		// TODO: file size is empty, then what?
		printf("File is empty.\n");
		fflush(stdout);
		exit(0);
	}

	// allocate memory for the buffer. No need for extra
	// room for a null string terminator, since we're possibly
	// reading binary data only anyway (which can contain 0x00).
	char* contents = malloc(sizeof(char) * size);

	if (fread(contents, size, 1, fp) <= 0) {
		perror("Unable to read file contents");
		free(contents);
		exit(1);
	}

	// duplicate string without using gnu99 strdup().
	e->filename = malloc(strlen(filename) + 1);
	strncpy(e->filename, filename, strlen(filename) + 1);
	e->contents = contents;
	e->content_length = size;

	// Check if the file is readonly, and warn the user about that.
	if (access(filename, W_OK) == -1) {
		editor_statusmessage(e, STATUS_WARNING, "\"%s\" (%d bytes) [readonly]", e->filename, e->content_length);
	} else {
		editor_statusmessage(e, STATUS_INFO, "\"%s\" (%d bytes)", e->filename, e->content_length);
	}

	fclose(fp);
}

/**
 * Writes the contents of the editor's buffer the to the same filename.
 */
void editor_writefile(struct editor* e) {
	assert(e->filename != NULL);

	FILE* fp = fopen(e->filename, "wb");
	if (fp == NULL) {
		editor_statusmessage(e, STATUS_ERROR, "Unable to open '%s' for writing: %s", e->filename, strerror(errno));
		return;
	}

	size_t bw = fwrite(e->contents, sizeof(char), e->content_length, fp);
	if (bw <= 0) {
		editor_statusmessage(e, STATUS_ERROR, "Unable write to file: %s", strerror(errno));
		return;
	}

	editor_statusmessage(e, STATUS_INFO, "\"%s\", %d bytes written", e->filename, e->content_length);

	fclose(fp);
}

/**
 * Finds the cursor position at the given offset, taking the lines into account.
 * The result is set to the pointers `x' and `y'. We can therefore 'misuse' this
 * to set the cursor position of the editor to a given offset.
 *
 * Note that this function will NOT scroll the editor to the proper line.
 */
void editor_cursor_at_offset(struct editor* e, int offset, int* x, int* y) {
	*x = offset % e->octets_per_line + 1;
	*y = offset / e->octets_per_line - e->line + 1;
}

/**
 * Deletes the character (byte) at the current cursor position (in other
 * words, the current offset the cursor is at).
 */
void editor_delete_char_at_cursor(struct editor* e) {
	int offset = editor_offset_at_cursor(e);
	int old_length = e->content_length;

	if (e->content_length <= 0) {
		editor_statusmessage(e, STATUS_WARNING, "Nothing to delete");
		return;
	}

	// FIXME: when all chars have been removed from a file, this blows up.

	// Remove an element from the contents buffer by moving memory.
	// The character at the current offset is supposed to be removed.
	// Take the offset + 1, until the end of the buffer. Copy that
	// part over the offset, reallocate the contents buffer with one
	// character in size less.
	memmove(e->contents + offset, e->contents + offset + 1 , e->content_length - offset - 1);
	e->contents = realloc(e->contents, e->content_length - 1);
	e->content_length--;

	// if the deleted offset was the maximum offset, move the cursor to
	// the left.
	if (offset >= old_length - 1) {
		editor_move_cursor(e, KEY_LEFT, 1);
	}
}

/**
 * Exits the editor, frees some stuff and resets the terminal setting.
 */
void editor_exit() {
	editor_free(g_ec);
	clear_screen();
	disable_raw_mode();
}

void editor_increment_byte(struct editor* e, int amount) {
	int offset = editor_offset_at_cursor(e);
	e->contents[offset] += amount;
}

/**
 * Gets the current offset at which the cursor is.
 */
inline int editor_offset_at_cursor(struct editor* e) {
	// Calculate the offset based on the cursors' x and y coord (which is bound
	// between (1 .. line width) and (1 .. max screen rows). Take the current displayed
	// line into account (which is incremented when we are paging the content).
	// Multiply it by octets_per_line since we're effectively addressing a one dimensional
	// array.
	int offset = (e->cursor_y - 1 + e->line) * e->octets_per_line + (e->cursor_x - 1);
	// Safety measure. Since we're using the value of this function to
	// index the content array, we must not go out of bounds.
	if (offset <= 0) {
		return 0;
	}
	if (offset >= e->content_length) {
		return e->content_length - 1;
	}
	return offset;
}

/**
 * Scrolls the editor by updating the `line' accordingly, within
 * the bounds of the readable parts of the buffer.
 */
void editor_scroll(struct editor* e, int units) {
	e->line += units;

	// If we wanted to scroll past the end of the file, calculate the line
	// properly. Subtract the amount of screen rows (minus 2??) to prevent
	// scrolling past the end of file.
	int upper_limit = e->content_length / e->octets_per_line - (e->screen_rows - 2);
	if (e->line >= upper_limit) {
		e->line = upper_limit;
	}

	// If we scroll past the beginning of the file (offset 0 of course),
	// set our line to zero and return. This particular condition is also
	// necessary when the upper_limit calculated goes negative, because
	// This is either some weird calculation failure from my part, but
	// this seems to work. Failing to cap this will result in bad addressing
	// of the content in render_contents().
	if (e->line <= 0) {
		e->line = 0;
	}
}

void editor_setmode(struct editor* e, enum editor_mode mode) {
	e->mode = mode;
	switch (e->mode) {
	case MODE_NORMAL:  editor_statusmessage(e, STATUS_INFO, ""); break;
	case MODE_INSERT:  editor_statusmessage(e, STATUS_INFO, "-- INSERT --"); break;
	case MODE_REPLACE: editor_statusmessage(e, STATUS_INFO, "-- REPLACE --"); break;
	case MODE_COMMAND: return; // nothing.
	}
}

/**
 * Sets statusmessage, including color depending on severity.
 */
int editor_statusmessage(struct editor* e, enum status_severity sev, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int x = vsnprintf(e->status_message, sizeof(e->status_message), fmt, ap);
	va_end(ap);

	e->status_severity = sev;

	return x;
}

/**
 * Renders the given ASCII string, `asc' to the buffer `b'. The `rownum'
 * specified should be the row number being rendered in an iteration in
 * editor_render_contents. This function will render the selected byte
 * with a different color in the ASCII row to easily identify which
 * byte is being highlighted.
 */
void editor_render_ascii(struct editor* e, int rownum, const char* asc, struct charbuf* b) {
	assert(rownum > 0);

	// If the rownum given is the current y cursor position, then render the line
	// differently from the rest.
	if (rownum == e->cursor_y) {
		// Check the cursor position on the x axis
		for (int i = 0; i < strlen(asc); i++) {
			char x[1];
			if (i+1 == e->cursor_x) {
				// Highlight by 'inverting' the color
				charbuf_append(b, "\x1b[30;47m", 8);
			} else {
				// Other characters with greenish
				charbuf_append(b, "\x1b[32;40;1m", 10);
			}
			x[0] = asc[i];
			charbuf_append(b, x, 1);
		}
	} else {
		charbuf_append(b, "\x1b[1;37m", 7);
		charbuf_append(b, asc, strlen(asc));
	}
}


/**
 * Renders the contents of the current state of the editor `e'
 * to the buffer `b'.
 */
void editor_render_contents(struct editor* e, struct charbuf* b) {
	if (e->content_length <= 0) {
		// TODO: handle this in a better way.
		charbuf_append(b, "\x1b[2J", 4);
		charbuf_append(b, "empty", 5);
		return;
	}

	// FIXME: proper sizing of these arrays (malloc?)
	char address[80];  // example: 000000040
	char hex[ 2 + 1];  // example: 65
	char asc[256 + 1]; // example: Hello.World!

	// Counter to indicate how many chars have been written for the current
	// row of data. This is used for later for padding, when the iteration
	// is over, but there's still some ASCII to write.
	int row_char_count = 0;

	// start_offset is to determine where we should start reading from
	// the buffer. This is dependent on where the cursor is, and on the
	// octets which are visible per line.
	int start_offset = e->line * e->octets_per_line;
	if (start_offset >= e->content_length) {
		start_offset = e->content_length - e->octets_per_line;
	}

	// Determine the end offset for displaying. There is only so much
	// to be displayed 'per screen'. I.e. if you can only display 1024
	// bytes, you only have to read a maximum of 1024 bytes.
	int bytes_per_screen = e->screen_rows * e->octets_per_line;
	int end_offset = bytes_per_screen + start_offset - e->octets_per_line;
	if (end_offset > e->content_length) {
		end_offset = e->content_length;
	}

	int offset;
	int row = 0;
	for (offset = start_offset; offset < end_offset; offset++) {
		if (offset % e->octets_per_line == 0) {
			// start of a new row, beginning with an offset address in hex.
			int bwritten = snprintf(address, sizeof(address), "\e[0;33m%09x\e[0m:", offset);
			charbuf_append(b, address, bwritten);
			// Initialize the ascii buffer to all zeroes, and reset the row char count.
			memset(asc, 0, sizeof(asc));
			row_char_count = 0;
			row++;
		}

		// Format a hex string of the current character in the offset.
		snprintf(hex, sizeof(hex), "%02x", (unsigned char) e->contents[offset]);

		// Every iteration, set the ascii value in the buffer, until
		// 16 bytes are set. This will be written later when the hex
		// values are drawn to screen.
		if (isprint(e->contents[offset])) {
			asc[offset % e->octets_per_line] = e->contents[offset];
		} else {
			// non-printable characters are represented by a dot.
			asc[offset % e->octets_per_line] = '.';
		}

		// Every 'group' count, write a separator space.
		if (offset % e->grouping == 0) {
			charbuf_append(b, " ", 1);
			row_char_count++;
		}

		// First, write the hex value of the byte at the current offset.
		charbuf_append(b, hex, 2);
		row_char_count += 2;

		// If we reached the end of a 'row', start writing the ASCII equivalents
		// of the 'row'. Highlight the current line and offset on the ASCII part.
		if ((offset+1) % e->octets_per_line == 0) {
			charbuf_append(b, "  ", 2);
			editor_render_ascii(e, row, asc, b);
			charbuf_append(b, "\r\n", 2);
		}
	}

	// Check remainder of the last offset. If its bigger than zero,
	// we got a last line to write (ASCII only).
	if (offset % e->octets_per_line > 0) {
		// Padding characters, to align the ASCII properly. For example, this
		// could be the output at the end of the file:
		// 000000420: 0a53 4f46 5457 4152 452e 0a              .SOFTWARE..
		//                                       ^^^^^^^^^^^^
		//                                       padding chars
		int padding_size = (e->octets_per_line * 2) + (e->octets_per_line / e->grouping) - row_char_count;
		char* padding = malloc(padding_size * sizeof(char));
		memset(padding, ' ', padding_size);
		charbuf_append(b, padding, padding_size);
		charbuf_append(b, "\x1b[0m  ", 6);
		// render cursor on the ascii when applicable.
		editor_render_ascii(e, row, asc, b);
		free(padding);
	}

	// clear everything up until the end
	charbuf_append(b, "\x1b[0J", 4);

#ifndef NDEBUG
	int len = 256;
	char debug[len];
	memset(debug, 0, len);
	snprintf(debug, len, "\x1b[0m\x1b[37m\x1b[1;80HRows: %d, start offset: %09x, end offset: %09x", e->screen_rows, start_offset, end_offset);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	snprintf(debug, len, "\e[2;80H(cur_y,cur_x)=(%d,%d)", e->cursor_y, e->cursor_x);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	snprintf(debug, len, "\e[3;80HHex line width: %d", e->hex_line_width);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	int curr_offset = editor_offset_at_cursor(e);
	snprintf(debug, len, "\e[4;80H\e[0KLine: %d, cursor offset: %d (hex: %02x)", e->line, curr_offset, (unsigned char) e->contents[curr_offset]);
	charbuf_append(b, debug, len);

	memset(debug, 0, len);
	int xx;
	int yy;
	editor_cursor_at_offset(e, curr_offset, &xx, &yy);
	snprintf(debug, len, "\e[5;80H\e[0Kyy,xx = %d, %d", yy, xx);
	charbuf_append(b, debug, len);
#endif
}

/*
 * Renders a ruler at the bottom right part of the screen, containing
 * the current offset in hex and in base 10, the byte at the current
 * cursor position, and how far the cursor is in the file (as a percentage).
 */
void editor_render_ruler(struct editor* e, struct charbuf* b) {
	// Nothing to see. No address, no byte, no percentage. It's all a plain
	// dark void right now. Oblivion. No data to see here, move along.
	if (e->content_length <= 0) {
		return;
	}

	char rulermsg[80]; // buffer for the actual message.
	char buf[20];      // buffer for the cursor positioning

	int offset_at_cursor = editor_offset_at_cursor(e);
	unsigned char val = e->contents[offset_at_cursor];
	int percentage = (float)(offset_at_cursor + 1) / (float)e->content_length * 100;

	// Create a ruler string. We need to calculate the amount of bytes
	// we've actually written, to subtract that from the screen_cols to
	// align the string properly.
	int rmbw = snprintf(rulermsg, sizeof(rulermsg),
			"0x%09x,%d (%02x)  %d%%",
			offset_at_cursor, offset_at_cursor, val, percentage);
	// Create a string for the buffer to position the cursor.
	int cpbw = snprintf(buf, sizeof(buf), "\x1b[0m\x1b[%d;%dH", e->screen_rows, e->screen_cols - rmbw);

	// First write the cursor string, followed by the ruler message.
	charbuf_append(b, buf, cpbw);
	charbuf_append(b, rulermsg, rmbw);
}

/**
 * Renders the status line to the buffer `b'.
 */
void editor_render_status(struct editor* e, struct charbuf* b) {
	// buf holds the string for the cursor movement.
	char buf[20];
	int bw = snprintf(buf, sizeof(buf), "\x1b[%d;0H", e->screen_rows);
	charbuf_append(b, buf, bw);

	// Set color, write status message, and reset the color after.
	switch (e->status_severity) {
	case STATUS_INFO:    charbuf_append(b, "\x1b[0;30;47m", 10); break; // black on white
	case STATUS_WARNING: charbuf_append(b, "\x1b[0;30;43m", 10); break; // black on yellow
	case STATUS_ERROR:   charbuf_append(b, "\x1b[1;37;41m", 10); break; // white on red
	//               bold/increased intensity__/ /  /
	//                   foreground color_______/  /
	//                      background color______/
	}

	charbuf_append(b, e->status_message, strlen(e->status_message));
	charbuf_append(b, "\x1b[0m", 4);
}

/**
 * Refreshes the screen. It uses a temporary buffer to write everything that's
 * eligible for display to an internal buffer, and then 'draws' it to the screen
 * in one call.
 */
void editor_refresh_screen(struct editor* e) {
	char buf[32]; // temp buffer for snprintf.
	int bw; // bytes written by snprintf.
	struct charbuf* b = charbuf_create();

	charbuf_append(b, "\x1b[?25l", 6);
	charbuf_append(b, "\x1b[H", 3); // move the cursor top left

	editor_render_contents(e, b);
	editor_render_status(e, b);

	// Ruler: move to the right of the screen etc.
	editor_render_ruler(e, b);

	// Position cursor. This is done by taking into account the current
	// cursor position (1 .. 40), and the amount of spaces to add due to
	// grouping.
	// TODO: this is currently a bit hacky and/or out of place.
	int curx = (e->cursor_x - 1) * 2; // times 2 characters to represent a byte in hex
	int spaces = curx / (e->grouping * 2); // determine spaces to add due to grouping.
	int cruft = curx + spaces + 12; // 12 = the size of the address + ": "
	bw = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", e->cursor_y, cruft);
	charbuf_append(b, buf, bw);
	charbuf_append(b, "\x1b[?25h", 6);

	charbuf_draw(b);
	charbuf_free(b);
}

void editor_insert(struct editor* e, char x) {
	// We are inserting a single character. Reallocate memory to contain
	// this extra byte.
	e->contents = realloc(e->contents, e->content_length + 1);
	// Set the last allocated index to the character x.
	e->contents[e->content_length] = x;
	// Increase the content length since we inserted a character.
	e->content_length++;
}

void editor_replace_byte(struct editor* e, char x) {
	int offset = editor_offset_at_cursor(e);
	e->contents[offset] = x;
	editor_move_cursor(e, KEY_RIGHT, 1);
}

/**
 * Processes a keypress accordingly.
 */
void editor_process_keypress(struct editor* e) {
	int c = read_key();
	if (c == -1) {
		return;
	}

	// Handle some keys, independent of mode we're in.
	switch (c) {
	case KEY_ESC: editor_setmode(e, MODE_NORMAL); return;
	case KEY_CTRL_Q:   exit(0); return;
	case KEY_CTRL_S:   editor_writefile(e); return;

	case KEY_UP:
	case KEY_DOWN:
	case KEY_RIGHT:
	case KEY_LEFT:     editor_move_cursor(e, c, 1); return;

	case KEY_HOME:     e->cursor_x = 1; return;
	case KEY_END:      e->cursor_x = e->octets_per_line; return;
	case KEY_PAGEUP:   editor_scroll(e, -(e->screen_rows) + 2); return;
	case KEY_PAGEDOWN: editor_scroll(e, e->screen_rows - 2); return;
	}

	// Handle commands when in normal mode.
	if (e->mode == MODE_NORMAL) {
		switch (c) {
		// vi(m) like movement:
		case 'h': editor_move_cursor(e, KEY_LEFT,  1); break;
		case 'j': editor_move_cursor(e, KEY_DOWN,  1); break;
		case 'k': editor_move_cursor(e, KEY_UP,    1); break;
		case 'l': editor_move_cursor(e, KEY_RIGHT, 1); break;
		case 'x':
			editor_delete_char_at_cursor(e);
			break;
		case 'i':
			editor_setmode(e, MODE_INSERT); break;
		case 'r':
			editor_setmode(e, MODE_REPLACE); break;
		case 'b':
			// Move one group back.
			editor_move_cursor(e, KEY_LEFT, e->grouping); break;
		case 'w':
			// Move one group further.
			editor_move_cursor(e, KEY_RIGHT, e->grouping); break;
		case 'G':
			// Scroll to the end, place the cursor at the end.
			editor_scroll(e, e->content_length);
			editor_cursor_at_offset(e, e->content_length-1, &e->cursor_x, &e->cursor_y);
			break;
		case 'g':
			// Read extra keypress
			c = read_key();
			if (c == 'g') {
				// scroll to the start, place cursor at start.
				e->line = 0;
				editor_cursor_at_offset(e, 0, &e->cursor_x, &e->cursor_y);
			}
			break;
		case ']':
			editor_increment_byte(e, 1);
			break;
		case '[':
			editor_increment_byte(e, -1);
		}

		// Command parsed, do not continue.
		return;
	}

	if (e->mode == MODE_INSERT) {
		// Insert character after the cursor.
		//editor_insert(e, (char) c);
	} else if (e->mode == MODE_REPLACE) {
		// Check if the input character was a valid hex value. If not, return prematurely.
		if (!ishex(c)) {
			editor_statusmessage(e, STATUS_ERROR, "'%c' is not valid hex", c);
			return;
		}
		int next = read_key();
		if (!ishex(next)) {
			editor_statusmessage(e, STATUS_ERROR, "'%c' is not valid hex", next);
			return;
		}

		int offset = editor_offset_at_cursor(e);
		char crud[2 + 1];
		snprintf(crud, sizeof(crud), "%c%c", (char)c, (char)next);
		char ashex = hex2bin(crud);
		editor_replace_byte(e, ashex);
		editor_statusmessage(e, STATUS_INFO, "Replaced byte at offset %09x with %02x", offset, (unsigned char)ashex);
	} else if (e->mode == MODE_COMMAND) {
		// Input manual, typed commands.
	}
}
/**
 * Initializes editor struct with some default values.
 */
struct editor* editor_init() {
	struct editor* e = malloc(sizeof(struct editor));

	e->octets_per_line = 16;
	e->grouping = 2;
	e->hex_line_width = e->octets_per_line * 2 + (e->octets_per_line / 2) - 1;

	e->line = 0;
	e->cursor_x = 1;
	e->cursor_y = 1;
	e->filename = NULL;
	e->contents = NULL;
	e->content_length = 0;

	memset(e->status_message, 0, sizeof(e->status_message));

	e->mode = MODE_NORMAL;

	get_window_size(&(e->screen_rows), &(e->screen_cols));

	return e;
}

void editor_free(struct editor* e) {
	free(e->filename);
	free(e->contents);
	free(e);
}


void debug_keypress() {
	char c;
	ssize_t nread;
	// check == 0 to see if EOF.
	while ((nread = read(STDIN_FILENO, &c, 1))) {
		if (c == 'q') { exit(1); };
		if (c == '\r' || c == '\n') { printf("\r\n"); continue; }
		if (isprint(c)) {
			printf("%c = %02x, ", c, c);
		} else {
			printf(". = %02x, ", c);
		}
		fflush(stdout);
	}
}

/**
 * Handles the SIGWINCH signal upon terminal resizing.
 */
void handle_term_resize(int sig) {
	clear_screen();
	get_window_size(&(g_ec->screen_rows), &(g_ec->screen_cols));
	editor_refresh_screen(g_ec);
}

int main(int argc, char* argv[]) {
	char* file = NULL;
	int octets_per_line = 16;
	int grouping = 4;

	int ch = 0;
	while ((ch = getopt(argc, argv, "vhg:o:")) != -1) {
		switch (ch) {
		case 'v':
			print_version();
			return 0;
		case 'h':
			print_help("");
			exit(0);
			break;
		case 'g':
			// parse grouping
			grouping = str2int(optarg, 2, 16, 4);
			break;
		case 'o':
			// parse octets per line
			octets_per_line = str2int(optarg, 16, 64, 16);
			break;
		default:
			print_help("");
			exit(1);
			break;
		}
	}


	// After all options are parsed, we expect a filename to open.
	if (optind >= argc) {
		print_help("error: expected filename\n");
		exit(1);
	}

	file = argv[optind];

	// Signal handler to react on screen resizing.
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = handle_term_resize;
	sigaction(SIGWINCH, &act, NULL);

	// Editor configuration passed around.
	g_ec = editor_init();
	g_ec->octets_per_line = octets_per_line;
	g_ec->grouping = grouping;

	editor_openfile(g_ec, file);

	enable_raw_mode();
	clear_screen();

	while (true) {
		editor_refresh_screen(g_ec);
		editor_process_keypress(g_ec);
		//debug_keypress();
	}

	editor_free(g_ec);
	return EXIT_SUCCESS;
}
