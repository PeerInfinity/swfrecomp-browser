// avm2_date.c — the AVM2 `Date` class (ECMA-262 §15.9 + the AS3 surface).
//
// Replaces the three-method stub that lived in avm2_amf.c (getTime/valueOf/
// toString, bolted on so AMF round-trips could carry a timestamp). The date
// math is the same ECMA-262 §15.9.1 algebra AVM1 uses
// (actionmodern/date.c); this file re-implements it against Avm2Value so the
// two runtimes stay independent.
//
// Three things about this file are load-bearing and easy to break:
//
//  1. EVERY prototype-facing method resolves its receiver through
//     avm2_date_ext_of(), which returns NULL for a non-Date. It must never
//     coerce. `Date.prototype.toString()` passes Date.prototype itself as the
//     receiver, and Date.prototype is a plain Object — a coercing helper would
//     call toString on it, re-entering this function until the stack dies.
//     That exact crash landed once already (see
//     avm2-prototype-toString-self-coercion in the investigation docs).
//
//  2. Arities registered on Date.prototype come from the corpus, not from
//     ECMA-262. They happen to agree for Date (all getters 0, setTime 1,
//     setMonth 2) but the rule still holds — see FLASH_BUGS_REPLICATED.md
//     §"[AVM2] Function.length on builtins deviates from ECMA-262".
//
//  3. Local time is a FIXED offset read once from the C library, with no DST
//     term. The test harness pins TZ=NPT-5:45 (Nepal, no DST) for
//     determinism, and the avmplus Date tests themselves assume a single
//     whole-run offset (they compute one TZ_DIFF from
//     `new Date(2000,1,1).getTimezoneOffset()` and adjust every hard-coded
//     PST result array by it). A per-timestamp DST lookup would disagree with
//     the tests in any zone that has DST, and agrees with this in any zone
//     that doesn't.
//
// getYear/setYear are deliberately absent: AS3 dropped them, and
// ecma3/Expressions/e11_2_1_1 asserts `typeof Date.prototype.getYear ==
// "undefined"`.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <avm2/avm2_class.h>
#include <avm2/avm2_error.h>
#include <avm2/avm2_globals.h>
#include <avm2/avm2_main.h>
#include <avm2/avm2_object.h>
#include <avm2/avm2_ops.h>

#ifndef MOCK_DATE_TIME
#define MOCK_DATE_TIME 981152406000ll
#endif

#define MS_PER_DAY 86400000.0
#define MS_PER_HOUR 3600000.0
#define MS_PER_MINUTE 60000.0
#define MS_PER_SECOND 1000.0

// ---------------------------------------------------------------------------
// ECMA-262 §15.9.1 day-number / time-value algebra
// ---------------------------------------------------------------------------

static double day_number(double t) { return floor(t / MS_PER_DAY); }

static double time_within_day(double t)
{
	double r = fmod(t, MS_PER_DAY);
	if (r < 0.0) r += MS_PER_DAY;
	return r + 0.0;
}

static int days_in_year(double y)
{
	double m4 = fmod(y, 4.0);
	if (m4 != 0.0) return 365;
	if (fmod(y, 100.0) != 0.0) return 366;
	if (fmod(y, 400.0) != 0.0) return 365;
	return 366;
}

static double day_from_year(double y)
{
	return 365.0 * (y - 1970.0) + floor((y - 1969.0) / 4.0)
	     - floor((y - 1901.0) / 100.0) + floor((y - 1601.0) / 400.0);
}

static double year_from_time(double t)
{
	double day = day_number(t);
	double y = floor(day / 365.2425) + 1970.0;
	while (day_from_year(y + 1.0) <= day) y += 1.0;
	while (day_from_year(y) > day) y -= 1.0;
	return y;
}

static int in_leap_year(double t)
{
	return days_in_year(year_from_time(t)) == 366 ? 1 : 0;
}

static double day_within_year(double t)
{
	return day_number(t) - day_from_year(year_from_time(t));
}

static const int MONTH_START[2][13] = {
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 },
};

static int month_from_time(double t)
{
	double dwy = day_within_year(t);
	int leap = in_leap_year(t);
	for (int m = 11; m >= 0; m--)
	{
		if (dwy >= (double) MONTH_START[leap][m]) return m;
	}
	return 0;
}

static double date_from_time(double t)
{
	int leap = in_leap_year(t);
	int m = month_from_time(t);
	return day_within_year(t) - (double) MONTH_START[leap][m] + 1.0;
}

static double week_day(double t)
{
	// 1970-01-01 was a Thursday (4).
	double d = fmod(day_number(t) + 4.0, 7.0);
	if (d < 0.0) d += 7.0;
	return d + 0.0;
}

static double hour_from_time(double t)
{
	double r = fmod(floor(t / MS_PER_HOUR), 24.0);
	if (r < 0.0) r += 24.0;
	return r + 0.0;
}

static double min_from_time(double t)
{
	double r = fmod(floor(t / MS_PER_MINUTE), 60.0);
	if (r < 0.0) r += 60.0;
	return r + 0.0;
}

static double sec_from_time(double t)
{
	double r = fmod(floor(t / MS_PER_SECOND), 60.0);
	if (r < 0.0) r += 60.0;
	return r + 0.0;
}

static double ms_from_time(double t)
{
	double r = fmod(t, MS_PER_SECOND);
	if (r < 0.0) r += MS_PER_SECOND;
	return floor(r) + 0.0;
}

// ToInteger, but NaN/Infinity pass through (MakeTime/MakeDay want them to).
static double to_integer(double x)
{
	if (isnan(x)) return NAN;
	if (isinf(x)) return x;
	return trunc(x);
}

static double make_time(double hour, double min, double sec, double ms)
{
	if (!isfinite(hour) || !isfinite(min) || !isfinite(sec) || !isfinite(ms))
	{
		return NAN;
	}
	return trunc(hour) * MS_PER_HOUR + trunc(min) * MS_PER_MINUTE
	     + trunc(sec) * MS_PER_SECOND + trunc(ms);
}

static double make_day(double year, double month, double date)
{
	if (!isfinite(year) || !isfinite(month) || !isfinite(date)) return NAN;
	year = trunc(year);
	month = trunc(month);
	date = trunc(date);
	double y = year + floor(month / 12.0);
	double m = fmod(month, 12.0);
	if (m < 0.0) m += 12.0;
	int mi = (int) m;
	int leap = days_in_year(y) == 366 ? 1 : 0;
	return day_from_year(y) + (double) MONTH_START[leap][mi] + date - 1.0;
}

static double make_date(double day, double time_v)
{
	if (!isfinite(day) || !isfinite(time_v)) return NAN;
	return day * MS_PER_DAY + time_v;
}

// ECMA-262 §15.9.1.14 TimeClip.
static double time_clip(double t)
{
	if (!isfinite(t)) return NAN;
	if (fabs(t) > 8.64e15) return NAN;
	return trunc(t) + 0.0;
}

// ---------------------------------------------------------------------------
// Local time zone (fixed offset — see the header comment)
// ---------------------------------------------------------------------------

static double g_local_tza_ms = 0.0;
static int g_tza_init = 0;

static void ensure_tza(void)
{
	if (g_tza_init) return;
	g_tza_init = 1;
	time_t now = time(NULL);
	struct tm gmt_tm, local_tm;
	if (gmtime_r(&now, &gmt_tm) == NULL || localtime_r(&now, &local_tm) == NULL)
	{
		return;
	}
	// timegm() reads the broken-down fields as if they were UTC, so the
	// difference of the two is exactly the zone offset.
	g_local_tza_ms = difftime(timegm(&local_tm), timegm(&gmt_tm)) * 1000.0;
}

static double local_time(double t)
{
	ensure_tza();
	return t + g_local_tza_ms;
}

static double utc_time(double t)
{
	ensure_tza();
	return t - g_local_tza_ms;
}

// ---------------------------------------------------------------------------
// Receiver access — never coercing (see header note 1)
// ---------------------------------------------------------------------------

Avm2DateExt* avm2_date_ext_of(Avm2Value v)
{
	if (v.kind != AVM2_VALUE_OBJECT || v.u.obj == NULL) return NULL;
	Avm2Context* ctx = avm2_get_context();
	if (ctx->builtins.date_class == NULL) return NULL;
	for (Avm2Class* c = v.u.obj->cls; c != NULL; c = c->super_class)
	{
		if (c == ctx->builtins.date_class)
		{
			return (Avm2DateExt*) v.u.obj->native_ext;
		}
	}
	return NULL;
}

// Borrowed-receiver state. Flash lets a plain object borrow the prototype
// methods and keep a working time value:
//
//   function MyDate(v) { this.setTime = Date.prototype.setTime;
//                        this.getTime = Date.prototype.getTime; }
//   var d = new MyDate(0); d.setTime(946684800000); d.getTime()  // -> the same
//
// (ecma3/Date/e15_9_5_23_3_rt asserts exactly that round-trip; the sibling
// ecma3/Exceptions/date_00{1,3,4}_rt assert that a borrowed receiver which was
// never set reads NaN / "Invalid Date".) So a non-Date object receiver keeps
// its time value in a hidden, non-enumerable own property.
//
// Both helpers below MUST stay raw own-slot accesses. The receiver of
// `Date.prototype.toString()` is Date.prototype itself, so anything that
// coerces the receiver — or walks a prototype chain into a getter — would
// recurse until the stack dies.
#define DATE_MILLIS_PROP "__avm2_date_millis__"
#define DATE_MILLIS_PROP_LEN ((uint32_t) (sizeof(DATE_MILLIS_PROP) - 1))

static double this_millis(Avm2Activation* act)
{
	Avm2DateExt* ext = avm2_date_ext_of(act->this_val);
	if (ext != NULL) return ext->millis;
	if (act->this_val.kind == AVM2_VALUE_OBJECT && act->this_val.u.obj != NULL)
	{
		Avm2Value* v = avm2_object_find_dynamic(act->this_val.u.obj,
		                                        DATE_MILLIS_PROP,
		                                        DATE_MILLIS_PROP_LEN);
		if (v != NULL && v->kind == AVM2_VALUE_NUMBER) return v->u.d;
	}
	return NAN;
}

static double store_millis(Avm2Activation* act, double t)
{
	Avm2DateExt* ext = avm2_date_ext_of(act->this_val);
	if (ext != NULL)
	{
		ext->millis = t;
	}
	else if (act->this_val.kind == AVM2_VALUE_OBJECT
	         && act->this_val.u.obj != NULL)
	{
		Avm2DynProp* p = avm2_object_set_dynamic(act->ctx,
		                                         act->this_val.u.obj,
		                                         DATE_MILLIS_PROP,
		                                         DATE_MILLIS_PROP_LEN,
		                                         avm2_number(t));
		if (p != NULL) p->dont_enum = 1;
	}
	return t;
}

static double arg_num(Avm2Activation* act, uint32_t i)
{
	if (i >= act->argc) return NAN;
	return avm2_coerce_to_number(act->ctx, act->args[i]);
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------

typedef enum
{
	FLD_FULLYEAR,
	FLD_MONTH,
	FLD_DATE,
	FLD_DAY,
	FLD_HOURS,
	FLD_MINUTES,
	FLD_SECONDS,
	FLD_MS,
} DateField;

static double field_of(double t, DateField f)
{
	switch (f)
	{
		case FLD_FULLYEAR: return year_from_time(t);
		case FLD_MONTH:    return (double) month_from_time(t);
		case FLD_DATE:     return date_from_time(t);
		case FLD_DAY:      return week_day(t);
		case FLD_HOURS:    return hour_from_time(t);
		case FLD_MINUTES:  return min_from_time(t);
		case FLD_SECONDS:  return sec_from_time(t);
		case FLD_MS:       return ms_from_time(t);
	}
	return NAN;
}

static Avm2Value get_field(Avm2Activation* act, DateField f, int utc)
{
	double t = this_millis(act);
	if (isnan(t)) return avm2_number(NAN);
	return avm2_number(field_of(utc ? t : local_time(t), f));
}

#define DATE_GETTER(fn_name, field, utc)                                       \
	static Avm2Value fn_name(Avm2Activation* act)                              \
	{                                                                          \
		return get_field(act, field, utc);                                     \
	}

DATE_GETTER(date_get_full_year,        FLD_FULLYEAR, 0)
DATE_GETTER(date_get_month,            FLD_MONTH,    0)
DATE_GETTER(date_get_date,             FLD_DATE,     0)
DATE_GETTER(date_get_day,              FLD_DAY,      0)
DATE_GETTER(date_get_hours,            FLD_HOURS,    0)
DATE_GETTER(date_get_minutes,          FLD_MINUTES,  0)
DATE_GETTER(date_get_seconds,          FLD_SECONDS,  0)
DATE_GETTER(date_get_milliseconds,     FLD_MS,       0)
DATE_GETTER(date_get_utc_full_year,    FLD_FULLYEAR, 1)
DATE_GETTER(date_get_utc_month,        FLD_MONTH,    1)
DATE_GETTER(date_get_utc_date,         FLD_DATE,     1)
DATE_GETTER(date_get_utc_day,          FLD_DAY,      1)
DATE_GETTER(date_get_utc_hours,        FLD_HOURS,    1)
DATE_GETTER(date_get_utc_minutes,      FLD_MINUTES,  1)
DATE_GETTER(date_get_utc_seconds,      FLD_SECONDS,  1)
DATE_GETTER(date_get_utc_milliseconds, FLD_MS,       1)

static Avm2Value date_get_time(Avm2Activation* act)
{
	return avm2_number(this_millis(act));
}

static Avm2Value date_get_timezone_offset(Avm2Activation* act)
{
	double t = this_millis(act);
	if (isnan(t)) return avm2_number(NAN);
	return avm2_number((t - local_time(t)) / MS_PER_MINUTE + 0.0);
}

// ---------------------------------------------------------------------------
// Setters
// ---------------------------------------------------------------------------
//
// ECMA-262 §15.9.5.23-.42 all share one shape: read the current broken-down
// fields (in local or UTC time), overwrite a contiguous run of them from the
// arguments actually supplied, and recombine. `first` names the leftmost
// field the method can set; `count` is how many it accepts.

typedef enum
{
	SET_YEAR,   // year, month, date
	SET_MONTH,  // month, date
	SET_DATE,   // date
	SET_HOURS,  // hours, min, sec, ms
	SET_MIN,    // min, sec, ms
	SET_SEC,    // sec, ms
	SET_MS,     // ms
} SetKind;

static Avm2Value date_set_fields(Avm2Activation* act, SetKind kind, int utc)
{
	double t = this_millis(act);

	// §15.9.5.40/.41: setFullYear alone rebuilds from +0 when the time value
	// is NaN. Every other setter propagates NaN.
	if (isnan(t))
	{
		if (kind != SET_YEAR)
		{
			// The arguments are still coerced (they can have side effects).
			for (uint32_t i = 0; i < act->argc; i++) (void) arg_num(act, i);
			return avm2_number(store_millis(act, NAN));
		}
		t = 0.0;
	}
	else if (!utc)
	{
		t = local_time(t);
	}

	double year = year_from_time(t);
	double month = (double) month_from_time(t);
	double date = date_from_time(t);
	double hours = hour_from_time(t);
	double minutes = min_from_time(t);
	double seconds = sec_from_time(t);
	double millis = ms_from_time(t);

	uint32_t i = 0;
	switch (kind)
	{
		case SET_YEAR:
			year = to_integer(arg_num(act, i++));
			if (i < act->argc) month = to_integer(arg_num(act, i++));
			if (i < act->argc) date = to_integer(arg_num(act, i++));
			break;
		case SET_MONTH:
			month = to_integer(arg_num(act, i++));
			if (i < act->argc) date = to_integer(arg_num(act, i++));
			break;
		case SET_DATE:
			date = to_integer(arg_num(act, i++));
			break;
		case SET_HOURS:
			hours = to_integer(arg_num(act, i++));
			if (i < act->argc) minutes = to_integer(arg_num(act, i++));
			if (i < act->argc) seconds = to_integer(arg_num(act, i++));
			if (i < act->argc) millis = to_integer(arg_num(act, i++));
			break;
		case SET_MIN:
			minutes = to_integer(arg_num(act, i++));
			if (i < act->argc) seconds = to_integer(arg_num(act, i++));
			if (i < act->argc) millis = to_integer(arg_num(act, i++));
			break;
		case SET_SEC:
			seconds = to_integer(arg_num(act, i++));
			if (i < act->argc) millis = to_integer(arg_num(act, i++));
			break;
		case SET_MS:
			millis = to_integer(arg_num(act, i++));
			break;
	}
	// Coerce any surplus arguments too, for side-effect parity.
	for (; i < act->argc; i++) (void) arg_num(act, i);

	double newv = make_date(make_day(year, month, date),
	                        make_time(hours, minutes, seconds, millis));
	if (!utc) newv = utc_time(newv);
	return avm2_number(store_millis(act, time_clip(newv)));
}

#define DATE_SETTER(fn_name, kind, utc)                                        \
	static Avm2Value fn_name(Avm2Activation* act)                              \
	{                                                                          \
		return date_set_fields(act, kind, utc);                                \
	}

DATE_SETTER(date_set_full_year,        SET_YEAR,  0)
DATE_SETTER(date_set_month,            SET_MONTH, 0)
DATE_SETTER(date_set_date,             SET_DATE,  0)
DATE_SETTER(date_set_hours,            SET_HOURS, 0)
DATE_SETTER(date_set_minutes,          SET_MIN,   0)
DATE_SETTER(date_set_seconds,          SET_SEC,   0)
DATE_SETTER(date_set_milliseconds,     SET_MS,    0)
DATE_SETTER(date_set_utc_full_year,    SET_YEAR,  1)
DATE_SETTER(date_set_utc_month,        SET_MONTH, 1)
DATE_SETTER(date_set_utc_date,         SET_DATE,  1)
DATE_SETTER(date_set_utc_hours,        SET_HOURS, 1)
DATE_SETTER(date_set_utc_minutes,      SET_MIN,   1)
DATE_SETTER(date_set_utc_seconds,      SET_SEC,   1)
DATE_SETTER(date_set_utc_milliseconds, SET_MS,    1)

static Avm2Value date_set_time(Avm2Activation* act)
{
	return avm2_number(store_millis(act, time_clip(arg_num(act, 0))));
}

// ---------------------------------------------------------------------------
// String formats
// ---------------------------------------------------------------------------

static const char* const DAY_NAMES[7] = { "Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat" };
static const char* const MONTH_NAMES[12] = { "Jan", "Feb", "Mar", "Apr",
                                             "May", "Jun", "Jul", "Aug",
                                             "Sep", "Oct", "Nov", "Dec" };

// Flash's zone suffix: "GMT+0545" / "GMT-0800".
static void format_zone(char* buf, size_t n)
{
	ensure_tza();
	double off_min = g_local_tza_ms / MS_PER_MINUTE;
	char sign = off_min < 0.0 ? '-' : '+';
	int abs_min = (int) fabs(off_min);
	snprintf(buf, n, "GMT%c%02d%02d", sign, abs_min / 60, abs_min % 60);
}

// Every one of these formats is pinned by a test, and the pin is almost
// always "Date.parse(d.toXString()) round-trips", not a literal comparison:
//
//   e15_9_5_2  toString   — Date.parse(now.toString()) within 1s of valueOf
//   e15_9_5_42 toUTCString— same, and exact for fixed timestamps
//   e15_9_5_5  toLocale*  — Date.parse(d.toLocaleString()) EXACTLY == d
//   e15_9_5_3  toDateString — parses back to local midnight
//   e15_9_5_4  toTimeString — equals toString with toDateString's prefix and
//                             the trailing year sliced off, so the two must
//                             agree character-for-character in the middle
//   e15_9_5_7  toLocaleTimeString — only asserts typeof/length; its
//                             "expected" value is the harness's own error
//                             sentinel, produced because toString does NOT
//                             begin with toDateString (year placement differs)
//
// So toLocaleString deliberately omits an AM/PM marker: our parser (like
// Flash's) has no token for one, and the round-trip is what is asserted.
typedef enum
{
	FMT_FULL,           // toString:           Sat Feb 3 04:05:06 GMT+0545 2001
	FMT_UTC,            // toUTCString:        Sat Feb 3 04:05:06 2001 UTC
	FMT_LOCALE,         // toLocaleString:     Sat Feb 3 2001 04:05:06
	FMT_DATE,           // toDateString:       Sat Feb 3 2001
	FMT_TIME,           // toTimeString:       04:05:06 GMT+0545
	FMT_LOCALE_TIME,    // toLocaleTimeString: 04:05:06
} DateFormat;

static Avm2Value format_date(Avm2Activation* act, DateFormat fmt)
{
	Avm2Context* ctx = act->ctx;
	double m = this_millis(act);
	if (isnan(m))
	{
		return avm2_string(avm2_string_from_literal(ctx, "Invalid Date"));
	}
	double t = (fmt == FMT_UTC) ? m : local_time(m);
	int wd = (int) week_day(t);
	int mo = month_from_time(t);
	int d = (int) date_from_time(t);
	double y = year_from_time(t);
	int hh = (int) hour_from_time(t);
	int mi = (int) min_from_time(t);
	int ss = (int) sec_from_time(t);
	char zone[16];
	format_zone(zone, sizeof(zone));

	char buf[128];
	switch (fmt)
	{
		case FMT_FULL:
			snprintf(buf, sizeof(buf), "%s %s %d %02d:%02d:%02d %s %.0f",
			         DAY_NAMES[wd], MONTH_NAMES[mo], d, hh, mi, ss, zone, y);
			break;
		case FMT_UTC:
			snprintf(buf, sizeof(buf), "%s %s %d %02d:%02d:%02d %.0f UTC",
			         DAY_NAMES[wd], MONTH_NAMES[mo], d, hh, mi, ss, y);
			break;
		case FMT_LOCALE:
			snprintf(buf, sizeof(buf), "%s %s %d %.0f %02d:%02d:%02d",
			         DAY_NAMES[wd], MONTH_NAMES[mo], d, y, hh, mi, ss);
			break;
		case FMT_DATE:
			snprintf(buf, sizeof(buf), "%s %s %d %.0f",
			         DAY_NAMES[wd], MONTH_NAMES[mo], d, y);
			break;
		case FMT_TIME:
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", hh, mi, ss, zone);
			break;
		case FMT_LOCALE_TIME:
			snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mi, ss);
			break;
	}
	return avm2_string(avm2_string_from_literal(ctx, buf));
}

#define DATE_FORMATTER(fn_name, fmt)                                           \
	static Avm2Value fn_name(Avm2Activation* act)                              \
	{                                                                          \
		return format_date(act, fmt);                                          \
	}

DATE_FORMATTER(date_to_string,             FMT_FULL)
DATE_FORMATTER(date_to_utc_string,         FMT_UTC)
DATE_FORMATTER(date_to_locale_string,      FMT_LOCALE)
DATE_FORMATTER(date_to_date_string,        FMT_DATE)
DATE_FORMATTER(date_to_time_string,        FMT_TIME)
DATE_FORMATTER(date_to_locale_time_string, FMT_LOCALE_TIME)

static Avm2Value date_value_of(Avm2Activation* act)
{
	return avm2_number(this_millis(act));
}

// ---------------------------------------------------------------------------
// Date.parse — Flash's whitespace-token parser
// ---------------------------------------------------------------------------
//
// Not the ECMA-262 format: Flash splits on whitespace and classifies each
// token independently (date, HH:MM:SS, weekday name, month name, GMT/UTC
// offset, or a bare number that is a year when >= 70 and a day otherwise).
// Setting the same field twice is an error, but a month name may overwrite an
// earlier month name — "Sat Jan Oct 30 77" parses as October. Year, month and
// day must all end up set. This mirrors Ruffle's parse_full_date, which was
// reverse-engineered from Flash Player; the avm2 suite's `date_parse` test
// pins every branch.

typedef struct
{
	double year, month, day, hour, minute, second;
	int has_year, has_month, has_day, has_time;
	int has_zone;
	double zone_ms;  // offset east of UTC, in ms
} ParsedDate;

static int parse_uint_exact(const char* s, size_t len, unsigned* out)
{
	if (len == 0) return 0;
	unsigned v = 0;
	for (size_t i = 0; i < len; i++)
	{
		if (s[i] < '0' || s[i] > '9') return 0;
		v = v * 10u + (unsigned) (s[i] - '0');
	}
	*out = v;
	return 1;
}

static int month_index(const char* s, size_t len)
{
	if (len != 3) return -1;
	for (int i = 0; i < 12; i++)
	{
		if (memcmp(s, MONTH_NAMES[i], 3) == 0) return i;
	}
	return -1;
}

static int is_day_name(const char* s, size_t len)
{
	if (len != 3) return 0;
	for (int i = 0; i < 7; i++)
	{
		if (memcmp(s, DAY_NAMES[i], 3) == 0) return 1;
	}
	return 0;
}

// YYYY/MM/DD, MM/DD/YYYY or Mon/DD/YYYY. Field widths are exact.
static int parse_slash_date(const char* s, size_t len, ParsedDate* out)
{
	const char* part[4];
	size_t plen[4];
	int n = 0;
	size_t start = 0;
	for (size_t i = 0; i <= len; i++)
	{
		if (i == len || s[i] == '/')
		{
			if (n == 4) return 0;
			part[n] = s + start;
			plen[n] = i - start;
			n++;
			start = i + 1;
		}
	}
	if (n != 3) return 0;

	unsigned a, b, c;
	if (plen[0] == 4)
	{
		if (plen[1] != 2 || plen[2] != 2) return 0;
		if (!parse_uint_exact(part[0], 4, &a)) return 0;
		if (!parse_uint_exact(part[1], 2, &b) || b == 0) return 0;
		if (!parse_uint_exact(part[2], 2, &c)) return 0;
		out->year = (double) a;
		out->month = (double) (b - 1);
		out->day = (double) c;
	}
	else if (plen[0] == 2)
	{
		if (plen[1] != 2 || plen[2] != 4) return 0;
		if (!parse_uint_exact(part[0], 2, &a) || a == 0) return 0;
		if (!parse_uint_exact(part[1], 2, &b)) return 0;
		if (!parse_uint_exact(part[2], 4, &c)) return 0;
		out->year = (double) c;
		out->month = (double) (a - 1);
		out->day = (double) b;
	}
	else if (plen[0] == 3)
	{
		int mon = month_index(part[0], 3);
		if (mon < 0) return 0;
		if (plen[1] != 2 || plen[2] != 4) return 0;
		if (!parse_uint_exact(part[1], 2, &b)) return 0;
		if (!parse_uint_exact(part[2], 4, &c)) return 0;
		out->year = (double) c;
		out->month = (double) mon;
		out->day = (double) b;
	}
	else
	{
		return 0;
	}
	return 1;
}

// HH:MM:SS, each component exactly two digits.
static int parse_hms(const char* s, size_t len, ParsedDate* out)
{
	if (len != 8 || s[2] != ':' || s[5] != ':') return 0;
	unsigned h, m, sec;
	if (!parse_uint_exact(s, 2, &h)) return 0;
	if (!parse_uint_exact(s + 3, 2, &m)) return 0;
	if (!parse_uint_exact(s + 6, 2, &sec)) return 0;
	out->hour = (double) h;
	out->minute = (double) m;
	out->second = (double) sec;
	return 1;
}

static double parse_full_date(const char* s, size_t slen)
{
	ParsedDate p;
	memset(&p, 0, sizeof(p));

	size_t i = 0;
	while (i < slen)
	{
		while (i < slen && (unsigned char) s[i] <= ' ') i++;
		if (i >= slen) break;
		size_t start = i;
		while (i < slen && (unsigned char) s[i] > ' ') i++;
		const char* tok = s + start;
		size_t len = i - start;

		ParsedDate scratch = p;
		if (parse_slash_date(tok, len, &scratch))
		{
			if (p.has_year || p.has_month || p.has_day) return NAN;
			p.year = scratch.year;
			p.month = scratch.month;
			p.day = scratch.day;
			p.has_year = p.has_month = p.has_day = 1;
		}
		else if (parse_hms(tok, len, &scratch))
		{
			if (p.has_time) return NAN;
			p.hour = scratch.hour;
			p.minute = scratch.minute;
			p.second = scratch.second;
			p.has_time = 1;
		}
		else if (is_day_name(tok, len))
		{
			// Weekday names carry no information.
		}
		else if (month_index(tok, len) >= 0)
		{
			// A second month name overwrites the first — deliberately no
			// duplicate check here (Flash quirk, pinned by date_parse).
			p.month = (double) month_index(tok, len);
			p.has_month = 1;
		}
		else if (len >= 3 && (memcmp(tok, "GMT", 3) == 0
		                      || memcmp(tok, "UTC", 3) == 0))
		{
			if (p.has_zone) return NAN;
			if (len == 3)
			{
				p.zone_ms = 0.0;
			}
			else if (len == 8)
			{
				unsigned hh, mm;
				if (!parse_uint_exact(tok + 4, 2, &hh)) return NAN;
				if (!parse_uint_exact(tok + 6, 2, &mm)) return NAN;
				double off = (double) hh * MS_PER_HOUR
				           + (double) mm * MS_PER_MINUTE;
				if (tok[3] == '-') p.zone_ms = -off;
				else if (tok[3] == '+') p.zone_ms = off;
				else return NAN;
			}
			else
			{
				return NAN;
			}
			p.has_zone = 1;
		}
		else
		{
			unsigned num;
			if (!parse_uint_exact(tok, len, &num)) return NAN;
			if (num >= 70)
			{
				if (p.has_year) return NAN;
				if (num < 100) num += 1900;
				p.year = (double) num;
				p.has_year = 1;
			}
			else
			{
				if (p.has_day) return NAN;
				p.day = (double) num;
				p.has_day = 1;
			}
		}
	}

	if (!p.has_year || !p.has_month || !p.has_day) return NAN;

	double t = make_date(make_day(p.year, p.month, p.day),
	                     make_time(p.hour, p.minute, p.second, 0.0));
	if (p.has_zone) t -= p.zone_ms;
	else t = utc_time(t);
	return time_clip(t);
}

static Avm2Value date_parse(Avm2Activation* act)
{
	if (act->argc == 0) return avm2_number(NAN);
	const Avm2String* s = avm2_coerce_to_string(act->ctx, act->args[0]);
	if (s == NULL) return avm2_number(NAN);
	return avm2_number(parse_full_date(s->utf8, s->len));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static double current_millis(void)
{
	return (double) (MOCK_DATE_TIME);
}

// ECMA-262 §15.9.3.1 step 9: a year in [0, 99] means 19xx.
static double apply_1900_window(double y)
{
	double yi = to_integer(y);
	if (!isnan(yi) && yi >= 0.0 && yi <= 99.0) return 1900.0 + yi;
	return y;
}

// Shared by the 2-to-7-argument constructor and Date.UTC: the only
// difference is whether the assembled fields are read as local or UTC.
static double components_to_millis(Avm2Activation* act, int utc)
{
	double year = apply_1900_window(arg_num(act, 0));
	double month = arg_num(act, 1);
	double date = act->argc > 2 ? arg_num(act, 2) : 1.0;
	double hours = act->argc > 3 ? arg_num(act, 3) : 0.0;
	double minutes = act->argc > 4 ? arg_num(act, 4) : 0.0;
	double seconds = act->argc > 5 ? arg_num(act, 5) : 0.0;
	double millis = act->argc > 6 ? arg_num(act, 6) : 0.0;
	// Surplus arguments still coerce (side-effect parity).
	for (uint32_t i = 7; i < act->argc; i++) (void) arg_num(act, i);

	double t = make_date(make_day(year, month, date),
	                     make_time(hours, minutes, seconds, millis));
	if (!utc) t = utc_time(t);
	return time_clip(t);
}

static void date_native_init(Avm2Context* ctx, Avm2Object* obj)
{
	(void) ctx;
	((Avm2DateExt*) obj->native_ext)->millis = current_millis();
}

static Avm2Value date_init(Avm2Activation* act)
{
	Avm2DateExt* ext = avm2_date_ext_of(act->this_val);
	if (ext == NULL) return avm2_undefined();

	if (act->argc == 0)
	{
		ext->millis = current_millis();
	}
	else if (act->argc == 1)
	{
		// Flash tests the RAW argument for String-ness, not its primitive:
		// `new Date({valueOf: function(){ return "Tue Feb 1 2005" }})` is NaN,
		// not a parsed date (pinned by the avm2 suite's `date` test).
		if (act->args[0].kind == AVM2_VALUE_STRING)
		{
			const Avm2String* s = act->args[0].u.str;
			ext->millis = s != NULL ? parse_full_date(s->utf8, s->len) : NAN;
		}
		else
		{
			ext->millis = time_clip(avm2_coerce_to_number(act->ctx,
			                                              act->args[0]));
		}
	}
	else
	{
		ext->millis = components_to_millis(act, 0);
	}
	return avm2_undefined();
}

// §15.9.2.1: `Date(...)` as a plain call ignores its arguments entirely and
// returns the current time as a string.
static Avm2Value date_class_call(Avm2Context* ctx, Avm2Class* cls,
                                 const Avm2Value* args, uint32_t argc)
{
	(void) cls;
	(void) args;
	(void) argc;
	Avm2Value now = avm2_class_construct(ctx, ctx->builtins.date_class, NULL, 0);
	Avm2Activation fake;
	memset(&fake, 0, sizeof(fake));
	fake.ctx = ctx;
	fake.this_val = now;
	return date_to_string(&fake);
}

static Avm2Value date_utc_static(Avm2Activation* act)
{
	return avm2_number(components_to_millis(act, 1));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// name, native fn, prototype arity (from the corpus — see header note 2).
typedef struct
{
	const char* name;
	Avm2MethodFn fn;
	uint32_t arity;
} DateMethod;

static const DateMethod DATE_METHODS[] = {
	{ "getTime",              date_get_time,              0 },
	{ "getFullYear",          date_get_full_year,         0 },
	{ "getMonth",             date_get_month,             0 },
	{ "getDate",              date_get_date,              0 },
	{ "getDay",               date_get_day,               0 },
	{ "getHours",             date_get_hours,             0 },
	{ "getMinutes",           date_get_minutes,           0 },
	{ "getSeconds",           date_get_seconds,           0 },
	{ "getMilliseconds",      date_get_milliseconds,      0 },
	{ "getUTCFullYear",       date_get_utc_full_year,     0 },
	{ "getUTCMonth",          date_get_utc_month,         0 },
	{ "getUTCDate",           date_get_utc_date,          0 },
	{ "getUTCDay",            date_get_utc_day,           0 },
	{ "getUTCHours",          date_get_utc_hours,         0 },
	{ "getUTCMinutes",        date_get_utc_minutes,       0 },
	{ "getUTCSeconds",        date_get_utc_seconds,       0 },
	{ "getUTCMilliseconds",   date_get_utc_milliseconds,  0 },
	{ "getTimezoneOffset",    date_get_timezone_offset,   0 },
	{ "setTime",              date_set_time,              1 },
	{ "setFullYear",          date_set_full_year,         3 },
	{ "setMonth",             date_set_month,             2 },
	{ "setDate",              date_set_date,              1 },
	{ "setHours",             date_set_hours,             4 },
	{ "setMinutes",           date_set_minutes,           3 },
	{ "setSeconds",           date_set_seconds,           2 },
	{ "setMilliseconds",      date_set_milliseconds,      1 },
	{ "setUTCFullYear",       date_set_utc_full_year,     3 },
	{ "setUTCMonth",          date_set_utc_month,         2 },
	{ "setUTCDate",           date_set_utc_date,          1 },
	{ "setUTCHours",          date_set_utc_hours,         4 },
	{ "setUTCMinutes",        date_set_utc_minutes,       3 },
	{ "setUTCSeconds",        date_set_utc_seconds,       2 },
	{ "setUTCMilliseconds",   date_set_utc_milliseconds,  1 },
	{ "toString",             date_to_string,             0 },
	{ "toDateString",         date_to_date_string,        0 },
	{ "toTimeString",         date_to_time_string,        0 },
	{ "toLocaleString",       date_to_locale_string,      0 },
	{ "toLocaleDateString",   date_to_date_string,        0 },
	{ "toLocaleTimeString",   date_to_locale_time_string, 0 },
	{ "toUTCString",          date_to_utc_string,         0 },
	{ "valueOf",              date_value_of,              0 },
};

// The AS3 accessor properties (Date.as): every one forwards to the method of
// the same meaning. `timezoneOffset`, `day` and `dayUTC` are read-only.
typedef struct
{
	const char* name;
	Avm2MethodFn getter;
	Avm2MethodFn setter;
} DateAccessor;

static const DateAccessor DATE_ACCESSORS[] = {
	{ "fullYear",         date_get_full_year,        date_set_full_year },
	{ "month",            date_get_month,            date_set_month },
	{ "date",             date_get_date,             date_set_date },
	{ "hours",            date_get_hours,            date_set_hours },
	{ "minutes",          date_get_minutes,          date_set_minutes },
	{ "seconds",          date_get_seconds,          date_set_seconds },
	{ "milliseconds",     date_get_milliseconds,     date_set_milliseconds },
	{ "fullYearUTC",      date_get_utc_full_year,    date_set_utc_full_year },
	{ "monthUTC",         date_get_utc_month,        date_set_utc_month },
	{ "dateUTC",          date_get_utc_date,         date_set_utc_date },
	{ "hoursUTC",         date_get_utc_hours,        date_set_utc_hours },
	{ "minutesUTC",       date_get_utc_minutes,      date_set_utc_minutes },
	{ "secondsUTC",       date_get_utc_seconds,      date_set_utc_seconds },
	{ "millisecondsUTC",  date_get_utc_milliseconds, date_set_utc_milliseconds },
	{ "time",             date_get_time,             date_set_time },
	{ "timezoneOffset",   date_get_timezone_offset,  NULL },
	{ "day",              date_get_day,              NULL },
	{ "dayUTC",           date_get_utc_day,          NULL },
};

void avm2_register_date(Avm2Context* ctx)
{
	Avm2Class* date = ctx->builtins.date_class;
	if (date == NULL) return;

	date->native_ext_size = sizeof(Avm2DateExt);
	date->native_init = date_native_init;
	date->native_call = date_class_call;
	date->instance_init.fn = date_init;
	date->instance_init.debug_name = "Date";

	for (size_t i = 0; i < sizeof(DATE_METHODS) / sizeof(DATE_METHODS[0]); i++)
	{
		const DateMethod* m = &DATE_METHODS[i];
		avm2_builtin_add_method_n(ctx, date, m->name, m->fn, m->arity);
		avm2_proto_add_function_n(ctx, date->prototype_obj, m->name, m->fn,
		                          m->arity);
	}
	for (size_t i = 0;
	     i < sizeof(DATE_ACCESSORS) / sizeof(DATE_ACCESSORS[0]); i++)
	{
		const DateAccessor* a = &DATE_ACCESSORS[i];
		avm2_builtin_add_getset(ctx, date, a->name, a->getter, a->setter);
	}

	// toJSON is prototype-only (Date.as declares no AS3::toJSON) and takes
	// the key argument JSON.stringify passes, which it ignores.
	avm2_proto_add_function_n(ctx, date->prototype_obj, "toJSON",
	                          date_to_string, 1);

	// Date.prototype is a Date instance in avmplus (its time value is NaN), so
	// a write to one of Date's method names resolves the ivtable trait and
	// throws #1037 -- which is the whole of ecma3/Date/e15_9_5. Unlike Array
	// and Boolean, Date's ES3 methods really are public traits there.
	Avm2Object* proto = date->prototype_obj;
	proto->cls = date;
	Avm2DateExt* proto_ext = avm2_alloc(ctx, sizeof(Avm2DateExt));
	proto_ext->millis = NAN;
	proto->native_ext = proto_ext;

	avm2_builtin_add_static_method(ctx, date, "parse", date_parse);
	avm2_builtin_add_static_method(ctx, date, "UTC", date_utc_static);
}
