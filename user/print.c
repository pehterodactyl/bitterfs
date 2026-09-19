#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "print.h"

/* Field name column.  One constant so every row lines up and changing the
 * layout is a single edit. */
#define NAME_W 16

void format_uuid(char *out, const bt_u8 *fsid)
{
	static const int dash_after[] = { 3, 5, 7, 9 };
	int di = 0;

	/* sprintf is safe only because the length is fixed by BITTER_FSID_SIZE:
	 * 32 hex digits + 4 dashes + NUL == PRINT_UUID_LEN. */
	for (int i = 0; i < BITTER_FSID_SIZE; i++) {
		out += sprintf(out, "%02x", fsid[i]);
		if (di < 4 && i == dash_after[di]) { *out++ = '-'; di++; }
	}
	*out = '\0';
}

void human(char *buf, size_t n, bt_u64 bytes)
{
	static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = (double)bytes;
	unsigned u = 0;

	while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
	if (u == 0) snprintf(buf, n, "%" PRIu64 " B", bytes);
	else        snprintf(buf, n, "%.1f %s", v, unit[u]);
}

void print_section(const char *title)
{
	size_t n = strlen(title);
	printf("\n%s\n", title);
	for (size_t i = 0; i < n; i++) putchar('-');
	putchar('\n');
}

void print_str(const char *name, const char *value)
{
	printf("  %-*s %s\n", NAME_W, name, value);
}

void print_u64(const char *name, bt_u64 value)
{
	printf("  %-*s %" PRIu64 "\n", NAME_W, name, value);
}

void print_u32(const char *name, bt_u32 value)
{
	printf("  %-*s %" PRIu32 "\n", NAME_W, name, value);
}

void print_hex64(const char *name, bt_u64 value)
{
	printf("  %-*s 0x%" PRIx64 "\n", NAME_W, name, value);
}

void print_size(const char *name, bt_u64 bytes)
{
	char h[16];
	human(h, sizeof h, bytes);
	printf("  %-*s %" PRIu64 " (%s)\n", NAME_W, name, bytes, h);
}

void print_uuid(const char *name, const bt_u8 *fsid)
{
	char u[PRINT_UUID_LEN];
	format_uuid(u, fsid);
	printf("  %-*s %s\n", NAME_W, name, u);
}

void print_bytes(const char *name, const bt_u8 *b, size_t n)
{
	printf("  %-*s ", NAME_W, name);
	for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
	putchar('\n');
}

int print_check(const char *what, int pass)
{
	/* Dots to the result so a long list stays readable when the names vary
	 * in length. */
	int used = printf("  %s ", what);
	for (int i = used; i < NAME_W + 30; i++) putchar('.');
	printf(" %s\n", pass ? "ok" : "FAIL");
	return pass;
}

void hexdump(const void *buf, size_t len, bt_u64 base)
{
	const bt_u8 *b = buf;

	for (size_t off = 0; off < len; off += 16) {
		size_t n = len - off < 16 ? len - off : 16;

		printf("  %08" PRIx64 "  ", base + off);
		for (size_t i = 0; i < 16; i++) {
			if (i < n) printf("%02x ", b[off + i]);
			else       fputs("   ", stdout);
			if (i == 7) putchar(' ');       /* split at the halfway mark */
		}
		fputs(" |", stdout);
		for (size_t i = 0; i < n; i++) {
			bt_u8 c = b[off + i];
			putchar(c >= 0x20 && c < 0x7f ? c : '.');
		}
		fputs("|\n", stdout);
	}
}


