/* searchutils.c - helper subroutines for grep's matchers.
   Copyright 1992, 1998, 2000, 2007, 2009-2014 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street - Fifth Floor, Boston, MA
   02110-1301, USA.  */

#include <config.h>
#include <assert.h>
#include "search.h"
#include "dfa.h"
#if HAVE_LANGINFO_CODESET
# include <langinfo.h>
#endif

#define NCHAR (UCHAR_MAX + 1)

static size_t mbclen_cache[NCHAR];

void
kwsinit (kwset_t *kwset)
{
  static char trans[NCHAR];
  int i;

  if (match_icase && MB_CUR_MAX == 1)
    {
      for (i = 0; i < NCHAR; ++i)
        trans[i] = toupper (i);

      *kwset = kwsalloc (trans);
    }
  else
    *kwset = kwsalloc (NULL);

  if (!*kwset)
    xalloc_die ();
}

#if MBS_SUPPORT
/* Convert BEG, an *N-byte string, to uppercase, and write the
   NUL-terminated result into malloc'd storage.  Upon success, set *N
   to the length (in bytes) of the resulting string (not including the
   trailing NUL byte), and return a pointer to the uppercase string.
   Upon memory allocation failure, exit.  *N must be positive.

   Although this function returns a pointer to malloc'd storage,
   the caller must not free it, since this function retains a pointer
   to the buffer and reuses it on any subsequent call.  As a consequence,
   this function is not thread-safe.

   When each character in the uppercase result string has the same length
   as the corresponding character in the input string, set *LEN_MAP_P
   to NULL.  Otherwise, set it to a malloc'd buffer (like the returned
   buffer, this must not be freed by caller) of the same length as the
   result string.  (*LEN_MAP_P)[J] is the change in byte-length of the
   character in BEG that formed byte J of the result as it was converted to
   uppercase.  It is usually zero.  For lowercase Turkish dotless I it
   is -1, since the lowercase input occupies two bytes, while the
   uppercase output occupies only one byte.  For lowercase I in the
   tr_TR.utf8 locale, it is 1 because the uppercase Turkish dotted I
   is one byte longer than the original.  When that happens, we have two
   or more slots in *LEN_MAP_P for each such character.  We store the
   difference in the first one and 0's in any remaining slots.

   This map is used by the caller to convert offset,length pairs that
   reference the uppercase result to numbers that refer to the matched
   part of the original buffer.  */

char *
mbtoupper (const char *beg, size_t *n, mb_len_map_t **len_map_p)
{
  static char *out;
  static mb_len_map_t *len_map;
  static size_t outalloc;
  size_t outlen, mb_cur_max;
  mbstate_t is, os;
  const char *end;
  char *p;
  mb_len_map_t *m;
  bool lengths_differ = false;

  if (*n > outalloc || outalloc == 0)
    {
      outalloc = MAX (1, *n);
      out = xrealloc (out, outalloc);
      len_map = xrealloc (len_map, outalloc);
    }

  /* appease clang-2.6 */
  assert (out);
  assert (len_map);
  if (*n == 0)
    return out;

  memset (&is, 0, sizeof (is));
  memset (&os, 0, sizeof (os));
  end = beg + *n;

  mb_cur_max = MB_CUR_MAX;
  p = out;
  m = len_map;
  outlen = 0;
  while (beg < end)
    {
      wchar_t wc;
      size_t mbclen = mbrtowc (&wc, beg, end - beg, &is);
#ifdef __CYGWIN__
      /* Handle a UTF-8 sequence for a character beyond the base plane.
         Cygwin's wchar_t is UTF-16, as in the underlying OS.  This
         results in surrogate pairs which need some extra attention.  */
      wint_t wci = 0;
      if (mbclen == 3 && (wc & 0xdc00) == 0xd800)
        {
          /* We got the start of a 4 byte UTF-8 sequence.  This is returned
             as a UTF-16 surrogate pair.  The first call to mbrtowc returned 3
             and wc has been set to a high surrogate value, now we're going
             to fetch the matching low surrogate.  This second call to mbrtowc
             is supposed to return 1 to complete the 4 byte UTF-8 sequence.  */
          wchar_t wc_2;
          size_t mbclen_2 = mbrtowc (&wc_2, beg + mbclen, end - beg - mbclen,
                                     &is);
          if (mbclen_2 == 1 && (wc_2 & 0xdc00) == 0xdc00)
            {
              /* Match.  Convert this to a 4 byte wint_t which constitutes
                 a 32-bit UTF-32 value.  */
              wci = ( (((wint_t) (wc - 0xd800)) << 10)
                     | ((wint_t) (wc_2 - 0xdc00)))
                    + 0x10000;
              ++mbclen;
            }
          else
            {
              /* Invalid UTF-8 sequence.  */
              mbclen = (size_t) -1;
            }
        }
#endif
      if (outlen + mb_cur_max >= outalloc)
        {
          size_t dm = m - len_map;
          out = x2nrealloc (out, &outalloc, 1);
          len_map = xrealloc (len_map, outalloc);
          p = out + outlen;
          m = len_map + dm;
        }

      if (mbclen == (size_t) -1 || mbclen == (size_t) -2 || mbclen == 0)
        {
          /* An invalid sequence, or a truncated multi-octet character.
             We treat it as a single-octet character.  */
          *m++ = 0;
          *p++ = *beg++;
          outlen++;
          memset (&is, 0, sizeof (is));
          memset (&os, 0, sizeof (os));
        }
      else
        {
          size_t ombclen;
          beg += mbclen;
#ifdef __CYGWIN__
          /* Handle Unicode characters beyond the base plane.  */
          if (mbclen == 4)
            {
              /* towupper, taking wint_t (4 bytes), handles UCS-4 values.  */
              wci = towupper (wci);
              if (wci >= 0x10000)
                {
                  wci -= 0x10000;
                  wc = (wci >> 10) | 0xd800;
                  /* No need to check the return value.  When reading the
                     high surrogate, the return value will be 0 and only the
                     mbstate indicates that we're in the middle of reading a
                     surrogate pair.  The next wcrtomb call reading the low
                     surrogate will then return 4 and reset the mbstate.  */
                  wcrtomb (p, wc, &os);
                  wc = (wci & 0x3ff) | 0xdc00;
                }
              else
                {
                  wc = (wchar_t) wci;
                }
              ombclen = wcrtomb (p, wc, &os);
            }
          else
#endif
          ombclen = wcrtomb (p, towupper (wc), &os);
          *m = mbclen - ombclen;
          memset (m + 1, 0, ombclen - 1);
          m += ombclen;
          p += ombclen;
          outlen += ombclen;
          lengths_differ |= (mbclen != ombclen);
        }
    }

  *len_map_p = lengths_differ ? len_map : NULL;
  *n = p - out;
  *p = 0;
  return out;
}

/* Initialize a cache of mbrlen values for each of its 1-byte inputs.  */
void
build_mbclen_cache (void)
{
  int i;

  for (i = CHAR_MIN; i <= CHAR_MAX; ++i)
    {
      char c = i;
      unsigned char uc = i;
      mbstate_t mbs = { 0 };
      mbclen_cache[uc] = mbrlen (&c, 1, &mbs);
    }
}

bool
is_mb_middle (const char **good, const char *buf, const char *end,
              size_t match_len)
{
  const char *p = *good;
  const char *prev = p;
  mbstate_t cur_state;

  if (using_utf8 () && buf - p > MB_CUR_MAX)
    {
      for (p = buf; buf - p > MB_CUR_MAX; p--)
        if (mbclen_cache[to_uchar (*p)] != (size_t) -1)
          break;

      if (buf - p == MB_CUR_MAX)
        p = buf;
    }

  memset (&cur_state, 0, sizeof cur_state);

  while (p < buf)
    {
      size_t mbclen = mbclen_cache[to_uchar (*p)];

      if (mbclen == (size_t) -2)
        mbclen = mbrlen (p, end - p, &cur_state);

      /* Store the beginning of the previous complete multibyte character.  */
      if (mbclen != (size_t) -2)
        prev = p;

      if (mbclen == (size_t) -1 || mbclen == (size_t) -2 || mbclen == 0)
        {
          /* An invalid sequence, or a truncated multibyte character.
             We treat it as a single byte character.  */
          mbclen = 1;
          memset (&cur_state, 0, sizeof cur_state);
        }
      p += mbclen;
    }

  *good = prev;

  if (p > buf)
    return true;

  /* P == BUF here.  */
  return 0 < match_len && match_len < mbrlen (p, end - p, &cur_state);
}
#endif /* MBS_SUPPORT */
