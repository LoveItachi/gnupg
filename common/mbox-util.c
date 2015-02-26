/* mbox-util.c - Mail address helper functions
 * Copyright (C) 1998-2010 Free Software Foundation, Inc.
 * Copyright (C) 1998-2015 Werner Koch
 *
 * This file is part of GnuPG.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of either
 *
 *   - the GNU Lesser General Public License as published by the Free
 *     Software Foundation; either version 3 of the License, or (at
 *     your option) any later version.
 *
 * or
 *
 *   - the GNU General Public License as published by the Free
 *     Software Foundation; either version 2 of the License, or (at
 *     your option) any later version.
 *
 * or both in parallel, as here.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "util.h"
#include "mbox-util.h"


static int
string_count_chr (const char *string, int c)
{
  int count;

  for (count=0; *string; string++ )
    if ( *string == c )
      count++;
  return count;
}


static int
string_has_ctrl_or_space (const char *string)
{
  for (; *string; string++ )
    if (!(*string & 0x80) && *string <= 0x20)
      return 1;
  return 0;
}


/* Return true if STRING has two consecutive '.' after an '@'
   sign.  */
static int
has_dotdot_after_at (const char *string)
{
  string = strchr (string, '@');
  if (!string)
    return 0; /* No at-sign.  */
  string++;
  return !!strstr (string, "..");
}


/* Check whether the string has characters not valid in an RFC-822
   address.  To cope with OpenPGP we ignore non-ascii characters
   so that for example umlauts are legal in an email address.  An
   OpenPGP user ID must be utf-8 encoded but there is no strict
   requirement for RFC-822.  Thus to avoid IDNA encoding we put the
   address verbatim as utf-8 into the user ID under the assumption
   that mail programs handle IDNA at a lower level and take OpenPGP
   user IDs as utf-8.  Note that we can't do an utf-8 encoding
   checking here because in keygen.c this function is called with the
   native encoding and native to utf-8 encoding is only done  later.  */
int
has_invalid_email_chars (const char *s)
{
  int at_seen=0;
  const char *valid_chars=
    "01234567890_-.abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  for ( ; *s; s++ )
    {
      if ( (*s & 0x80) )
        continue; /* We only care about ASCII.  */
      if ( *s == '@' )
        at_seen=1;
      else if ( !at_seen && !(strchr (valid_chars, *s)
                              || strchr ("!#$%&'*+/=?^`{|}~", *s)))
        return 1;
      else if ( at_seen && !strchr( valid_chars, *s ) )
        return 1;
    }
  return 0;
}


/* Check whether NAME represents a valid mailbox according to
   RFC822. Returns true if so. */
int
is_valid_mailbox (const char *name)
{
  return !( !name
            || !*name
            || has_invalid_email_chars (name)
            || string_count_chr (name,'@') != 1
            || *name == '@'
            || name[strlen(name)-1] == '@'
            || name[strlen(name)-1] == '.'
            || strstr (name, "..") );
}


/* Return the mailbox (local-part@domain) form a standard user id.
   All plain ASCII characters in the result are converted to
   lowercase.  Caller must free the result.  Returns NULL if no valid
   mailbox was found (or we are out of memory). */
char *
mailbox_from_userid (const char *userid)
{
  const char *s, *s_end;
  size_t len;
  char *result = NULL;

  s = strchr (userid, '<');
  if (s)
    {
      /* Seems to be a standard user id.  */
      s++;
      s_end = strchr (s, '>');
      if (s_end && s_end > s)
        {
          len = s_end - s;
          result = xtrymalloc (len + 1);
          if (!result)
            return NULL; /* Ooops - out of core.  */
          strncpy (result, s, len);
          result[len] = 0;
          /* Apply some basic checks on the address.  We do not use
             is_valid_mailbox because those checks are too strict.  */
          if (string_count_chr (result, '@') != 1  /* Need exactly one '@.  */
              || *result == '@'           /* local-part missing.  */
              || result[len-1] == '@'     /* domain missing.  */
              || result[len-1] == '.'     /* ends with a dot.  */
              || string_has_ctrl_or_space (result)
              || has_dotdot_after_at (result))
            {
              xfree (result);
              result = NULL;
              errno = EINVAL;
            }
        }
      else
        errno = EINVAL;
    }
  else if (is_valid_mailbox (userid))
    {
      /* The entire user id is a mailbox.  Return that one.  Note that
         this fallback method has some restrictions on the valid
         syntax of the mailbox.  However, those who want weird
         addresses should know about it and use the regular <...>
         syntax.  */
      result = xtrystrdup (userid);
    }
  else
    errno = EINVAL;

  return result? ascii_strlwr (result): NULL;
}


/* Check whether UID is a valid standard user id of the form
     "Heinrich Heine <heinrichh@duesseldorf.de>"
   and return true if this is the case. */
int
is_valid_user_id (const char *uid)
{
  if (!uid || !*uid)
    return 0;

  return 1;
}
