/* mildly nonconformant C23 vsnprintf() sans floats, doubles, and wide
 * char/str support. constant stack space. no POSIX extensions, not LP64
 * clean.
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
	if(sign) val = -(long long)val;
	else if(p->forcesign || p->spacesign) {
		sc = p->forcesign ? '+' : ' ';
		sign = true;
	}
	while(val > 0 || pos == 0) {
		if(p->shift > 0) {
			d = val & mask;
			val >>= p->shift;
		} else {
			d = val % 10;
			val /= 10;
		}
		int c = last = "0123456789abcdef"[d];
		if(pos++ < max) buf[pos - 1] = last = p->upper ? toupper(c) : c;
		if(p->grouping && ++g == 3) {
			if(pos++ < max) buf[pos - 1] = '_';
			g = 0; last = '_';
		}
	}
	while(pos < p->precision) {
		if(pos++ < max) buf[pos - 1] = '0';
		last = '0';
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
	while(!p->leftjust && pos < width - np) {
		if(pos++ < max) buf[pos - 1] = p->pad;
	}
	for(int i=0; i < np && pos + i < max; i++) buf[pos + i] = prefix[i];
	pos += np;
	if(max > 0) {
		buf[min_t(int, pos, max)] = '\0';
		for(int i = 0, len = min_t(int, pos, max); i < len / 2; i++) {
			char *a = &buf[i], *b = &buf[len - 1 - i], t = *a;
			*a = *b;
			*b = t;
		}
	}
	if(p->leftjust) {
		while(pos < width) {
			if(pos++ < max) buf[pos - 1] = p->pad;
		}
		if(max > 0) buf[min_t(int, pos, max)] = '\0';
	}
	return pos;
}

int vsnprintf(char *restrict str, size_t size, const char *restrict fmt, va_list ap)
{
	static_assert(sizeof(int_fast8_t) == sizeof(int32_t));
	static_assert(sizeof(int_fast16_t) == sizeof(int32_t));
	assert(fmt != NULL);
	size_t pos = 0, max = str != NULL && size > 0 ? size - 1 : 0;
	if(max > 0) *str = '\0';
	for(size_t i = 0; fmt[i] != '\0'; i++) {
		size_t nb = strchrnul(fmt + i, '%') - (fmt + i);
		if(nb > 0) {
			/* unformatted section */
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
		bool dot = false, mod, widtharg = false, precarg = false, fast = false;
		int longness = 0, bits = 0;
		char type = ' ', *end;
		do {
			mod = true;
			switch(fmt[i]) {
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
		if(widtharg) {
			p.width = va_arg(ap, int);
			if(p.width < 0) {
				p.leftjust = true;
				p.width = -p.width;
			}
		}
		if(precarg) {
			int prec = va_arg(ap, int);
			if(prec >= 0) p.precision = prec;
		}
		/* conversion */
		switch(fmt[i]) {
			case 'X': p.upper = true; /* FALL THRU */
			case 'p': if(fmt[i] == 'p') { p.prefix = true; type = 'z'; } /* FALL THRU */
			case 'x': p.shift = 4;	/* FALL THRU */
			case 'o': if(fmt[i] == 'o') p.shift = 3;	/* FALL THRU */
			case 'b': if(fmt[i] == 'b') p.shift = 1;	/* FALL THRU */
			case 'u': p.is_signed = false; p.forcesign = false; p.spacesign = false; /* FALL THRU */
			case 'i': case 'd': {
				unsigned long long val;
				if(type == 'w') {
					if(bits & (bits - 1)) return -1;	/* not power of two */
					if(bits < 8 || bits > 64) return -1;
					type = bits < 64 ? 'd' : 'L';
				}
				switch(type) {
					case 'j': val = va_arg(ap, intmax_t); break;
					case 'z': val = va_arg(ap, ssize_t); break;
					case 't': val = va_arg(ap, ptrdiff_t); break;
					case 'c': case 'h': case 'd': case 'l':
						if(!p.is_signed) val = va_arg(ap, unsigned int); else val = va_arg(ap, int);
						break;
					case 'L': val = va_arg(ap, unsigned long long); break;
					default: val = -1;
				}
				pos += fmt_ull(pos < max ? str + pos : NULL, max - pos, val, &p);
				break;
			}
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
