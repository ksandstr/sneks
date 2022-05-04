/* mildly nonconformant C23 vsnprintf() sans floats, doubles, and wide
 * char/str support. constant stack space. calls malloc. no POSIX extensions,
 * not LP64 clean.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <ccan/minmax/minmax.h>

struct fmt_param {
	int shift, width, precision;
	bool is_signed, upper, grouping, forcesign, spacesign, leftjust, prefix;
	char pad;
};

static size_t fmt_ull(char *restrict buf, size_t max, unsigned long long val, const struct fmt_param *p)
{
	if(buf == NULL) max = 0;
	bool sign = p->is_signed && val > LLONG_MAX, zero = val == 0;
	int pos = 0, mask = (1 << p->shift) - 1, d, g = 0, sc = '-', width = max(p->width, 1), last = 0, np = 0;
	unsigned long long acc = val;
	if(sign) acc = -(long long)acc;
	else if(p->forcesign || p->spacesign) {
		sc = p->forcesign ? '+' : ' ';
		sign = true;
	}
	while(acc > 0 || pos == 0) {
		if(p->shift > 0) {
			d = acc & mask;
			acc >>= p->shift;
		} else {
			d = acc % 10;
			acc /= 10;
		}
		int c = last = "0123456789abcdef"[d];
		if(pos++ < max) buf[pos - 1] = last = p->upper ? toupper(c) : c;
		if(p->grouping && ++g == 3) {
			if(pos++ < max) buf[pos - 1] = '_';
			g = 0; last = '_';
		}
	}
	if(pos < p->precision) {
		last = '0';
		if(pos < max) memset(buf + pos, '0', min_t(size_t, max - pos + 1, p->precision - pos));
		pos = p->precision;
	}
	char prefix[3];
	if(p->prefix && !zero) {
		if(p->shift != 3) {
			assert(p->shift == 1 || p->shift == 4);
			int c = ".b..x"[p->shift];
			prefix[np++] = p->upper ? toupper(c) : c;
		}
		if(p->shift != 3 || last != '0') prefix[np++] = '0';
	}
	if(sign) prefix[np++] = sc;
	if(!p->leftjust && p->pad == ' ') {	/* prefix before space pad */
		for(int i=0; i < np && pos + i < max; i++) buf[pos + i] = prefix[i];
		pos += np; np = 0;
	}
	if(!p->leftjust && pos < width - np) {
		if(pos < max) memset(buf + pos, p->pad, min_t(size_t, max - pos + 1, width - np - pos));
		pos = width - np;
	}
	for(int i=0; i < np && pos + i < max; i++) buf[pos + i] = prefix[i];
	pos += np;
	if(max > 0 && pos > max) {
		/* TODO: replace w/ fancy analytical solution */
		char *truncbuf = malloc(pos + 1);
		fmt_ull(truncbuf, pos, val, p);
		memcpy(buf, truncbuf, max);
		buf[max] = '\0';
		free(truncbuf);
	} else if(max > 0) {
		buf[min_t(int, pos, max)] = '\0';
		for(int i = 0, len = min_t(int, pos, max); i < len / 2; i++) {
			char *a = &buf[i], *b = &buf[len - 1 - i], t = *a;
			*a = *b;
			*b = t;
		}
	}
	if(p->leftjust) {
		if(pos < width) {
			if(pos < max) memset(buf + pos, p->pad, min_t(size_t, max - pos + 1, width - pos));
			pos = width;
		}
		if(max > 0) buf[min_t(int, pos, max)] = '\0';
	}
	return pos;
}

int vsnprintf(char *restrict str, size_t size, const char *restrict fmt, va_list ap)
{
	static_assert(sizeof(int_fast8_t) == sizeof(int32_t) && sizeof(int_fast16_t) == sizeof(int32_t));
	assert(fmt != NULL);
	size_t pos = 0, max = str != NULL && size > 0 ? size - 1 : 0, nb;
	if(max > 0) *str = '\0';
	for(size_t i = 0; fmt[i] != '\0'; i++) {
		if(nb = strchrnul(fmt + i, '%') - (fmt + i), nb > 0) {
			/* literal section */
			if(pos < max) {
				memcpy(str + pos, fmt + i, min(max - pos, nb));
				str[min(pos + nb, max)] = '\0';
			}
			pos += nb;
			if(fmt[i += nb] == '\0') break;
		}
		assert(fmt[i] == '%');
		switch(fmt[++i]) {
			case '\0': return -1;
			case '%':
				if(pos++ < max) {
					str[pos - 1] = '%';
					str[min(pos, max)] = '\0';
				}
				continue;
		}
		/* accept modifiers */
		struct fmt_param p = { .pad = ' ', .width = -1, .precision = -1, .is_signed = true };
		int dot = false, mod, widtharg = false, precarg = false, fast = false, longness = 0, bits = 0, type = ' ';
		do {
			mod = true;
			switch(fmt[i]) {
				char *end;
				case 'l': if(longness < 0 || ++longness > 2) return -1; else break;
				case 'h': if(longness > 0 || --longness < -2) return -1; else break;
				case '#': p.prefix = true; break;
				case '0': if(!p.leftjust) p.pad = '0'; break;
				case '.': dot = true; break;
				case '*': if(dot) precarg = true; else widtharg = true; break;
				case '1' ... '9':
					*(dot ? &p.precision : &p.width) = strtoul(fmt + i, &end, 10);
					i = (end - fmt) - 1;
					break;
				case 'w':
					type = 'w';
					fast = fmt[++i] == 'f';
					bits = strtoul(fmt + i + fast, &end, 10);
					if(bits == 0 && end == fmt + i + fast) return -1;
					i = (end - fmt) - 1;
					break;
				case 'j': case 'z': case 't': type = fmt[i]; break;
				case '\'': p.grouping = true; break;
				case '-': p.pad = ' '; p.leftjust = true; break;
				case '+': p.forcesign = true; p.spacesign = false; break;
				case ' ': p.spacesign = !p.forcesign; break;
				default: mod = false; break;
			}
			if(mod && fmt[++i] == '\0') return -1;
		} while(mod);
		if(type == ' ') type = "chdlL"[longness + 2]; else if(longness != 0) return -1;
		if(widtharg && (p.width = va_arg(ap, int), p.width < 0)) {
			p.leftjust = true;
			p.width = -p.width;
		}
		if(precarg) {
			int prec = va_arg(ap, int);
			if(prec >= 0) p.precision = prec;
		}
		/* conversion */
		switch(fmt[i]) {
			unsigned long long val;
			case 'X': p.upper = true; /* FALL THRU */
			case 'p': if(fmt[i] == 'p') { p.prefix = true; type = 'z'; } /* FALL THRU */
			case 'x': p.shift = 4;	/* FALL THRU */
			case 'o': if(fmt[i] == 'o') p.shift = 3;	/* FALL THRU */
			case 'b': if(fmt[i] == 'b') p.shift = 1;	/* FALL THRU */
			case 'u': p.is_signed = false; p.forcesign = false; p.spacesign = false; /* FALL THRU */
			case 'i': case 'd':
				if(type == 'w') {
					if(bits & (bits - 1)) return -1;	/* not power of two */
					if(bits < 8 || bits > 64) return -1;
					type = bits < 64 ? 'd' : 'L';
				}
				switch(type) {
					case 'j': case 'L': val = va_arg(ap, unsigned long long); break;
					case 't': case 'z': case 'c': case 'h': case 'd': case 'l':
						if(!p.is_signed) val = va_arg(ap, unsigned int); else val = va_arg(ap, int);
						break;
					default: val = -1;
				}
				pos += fmt_ull(pos < max ? str + pos : NULL, max - pos, val, &p);
				break;
			case 'C': longness = 1;	/* FALL THRU */
			case 'c': {
				int c = (unsigned char)va_arg(ap, int);
				if(pos++ < max) str[pos - 1] = c;
				break;
			}
			case 'S': longness = 1; /* FALL THRU */
			case 's': {
				const char *src = va_arg(ap, const char *);
				if(src == NULL) src = "(nil)";
				ssize_t len = strnlen(src, p.precision >= 0 ? p.precision : SIZE_MAX);
				assert(p.precision < 0 || len <= p.precision);
				if(p.width > len) {
					if(pos < max) {
						size_t npad = min_t(ssize_t, p.width - len, max - pos);
						memset(str + pos, ' ', npad);
						str[pos + npad] = '\0';
					}
					pos += p.width - len;
				}
				if(pos < max) memcpy(str + pos, src, min_t(ssize_t, max - pos, len));
				pos += len;
				break;
			}
			case 'n': {
				void *p = va_arg(ap, void *);
				switch(type) {
					case 'j': *(intmax_t *)p = pos; break;
					case 'z': *(ssize_t *)p = pos; break;
					case 't': *(ptrdiff_t *)p = pos; break;
					case 'c': *(signed char *)p = pos; break;
					case 'h': *(short *)p = pos; break;
					case 'd': *(int *)p = pos; break;
					case 'l': *(long *)p = pos; break;
					case 'L': *(long long *)p = pos; break;
					case 'w':
						switch(bits + fast) {	/* now you know why you fear the night */
							case 8: *(int8_t *)p = pos; break;
							case 16: *(int16_t *)p = pos; break;
							case 32: case 33: case 9: case 17: *(int32_t *)p = pos; break;
							case 64: case 65: *(int64_t *)p = pos; break;
							default: return -1;
						}
						break;
				}
				break;
			}
			case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
				/* floats TODO */
			default: return -1;
		}
		if(max > 0) str[min(pos, max)] = '\0';
	}
	return pos;
}
