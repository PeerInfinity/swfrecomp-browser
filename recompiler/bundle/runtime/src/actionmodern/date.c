// date.c — Date object, constructor, prototype methods, timezone helpers.
//
// Carved out of action.c per
// SWFRecompDocs/plans/split-action-c-math-first.md (second subsystem,
// after Math). The public surface is in actionmodern/actiondate.h;
// everything else here is file-static.
//
// NOTE: MOCK_DATE_TIME is a per-test macro (#define'd from constants.h
// when a test pins the "now" timestamp). It's consumed inside
// actionDateConstruct / builtin_date_constructor via the same #ifdef
// guards the original code used; no header change is needed.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __has_include
#  if __has_include("constants.h")
#    include "constants.h"
#  endif
#endif

#include <actionmodern/action_internal.h>
#include <actionmodern/actiondate.h>

// ============================================================================
// Date class state
// ============================================================================

static ASFunction g_date_constructor;
static ASFunction g_date_funcs[40]; // 20 getters + 17 setters + toString + valueOf + UTC
static ASFunction g_date_utc_func;
// Bare constructor handle returned by ASnative(103, 256). Must have
// prototype_obj == NULL so `new ASnative(103, 256)()` produces a plain
// object (no Date.prototype chain), matching Ruffle's FunctionObject::table_native.
static ASFunction g_date_asnative_ctor_func;
static ASObject* g_date_prototype = NULL;
static int g_date_init_done = 0;
static double g_date_local_tza_ms = 0.0; // local timezone offset in ms (positive = east of UTC)
static int g_date_tza_init = 0;

#define DATE_MS_PER_DAY 86400000.0
#define DATE_MS_PER_HOUR 3600000.0
#define DATE_MS_PER_MINUTE 60000.0
#define DATE_MS_PER_SECOND 1000.0

// --- Date math utility functions (ECMAScript 3 algorithms) ---

static double date_day(double t) { return floor(t / DATE_MS_PER_DAY); }

static double date_time_within_day(double t) {
	double r = fmod(t, DATE_MS_PER_DAY);
	if (r < 0) r += DATE_MS_PER_DAY;
	return r + 0.0; // eliminate -0
}

static int date_days_in_year(int y) {
	if (y % 4 != 0) return 365;
	if (y % 100 != 0) return 366;
	if (y % 400 != 0) return 365;
	return 366;
}

static double date_day_from_year(double y) {
	return 365.0 * (y - 1970.0) + floor((y - 1969.0) / 4.0) - floor((y - 1901.0) / 100.0) + floor((y - 1601.0) / 400.0);
}

static double date_time_from_year(double y) {
	return DATE_MS_PER_DAY * date_day_from_year(y);
}

static double date_year_from_time(double t) {
	// Binary search for the year
	double day = date_day(t);
	// Approximate year
	double y = floor(day / 365.2425) + 1970.0;
	// Adjust: find the year such that day_from_year(y) <= day < day_from_year(y+1)
	while (date_day_from_year(y + 1) <= day) y++;
	while (date_day_from_year(y) > day) y--;
	return y;
}

static int date_in_leap_year(double t) {
	double y = date_year_from_time(t);
	return date_days_in_year((int)y) == 366 ? 1 : 0;
}

static int date_day_within_year(double t) {
	return (int)(date_day(t) - date_day_from_year(date_year_from_time(t)));
}

static const int date_month_start[2][13] = {
	{0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365}, // non-leap
	{0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}, // leap
};

static int date_month_from_time(double t) {
	int dwy = date_day_within_year(t);
	int leap = date_in_leap_year(t);
	for (int m = 11; m >= 0; m--) {
		if (dwy >= date_month_start[leap][m]) return m;
	}
	return 0;
}

static int date_date_from_time(double t) {
	int dwy = date_day_within_year(t);
	int leap = date_in_leap_year(t);
	int m = date_month_from_time(t);
	return dwy - date_month_start[leap][m] + 1;
}

static int date_week_day(double t) {
	double d = fmod(date_day(t) + 4.0, 7.0); // Jan 1 1970 = Thursday (4)
	if (d < 0) d += 7.0;
	return (int)d;
}

static double date_hour_from_time(double t) { return fmod(floor(t / DATE_MS_PER_HOUR), 24.0); }
static double date_min_from_time(double t) { return fmod(floor(t / DATE_MS_PER_MINUTE), 60.0); }
static double date_sec_from_time(double t) { return fmod(floor(t / DATE_MS_PER_SECOND), 60.0); }
static double date_ms_from_time(double t) { return floor(fmod(t, DATE_MS_PER_SECOND)); }

// Fix negative modular results and -0 for time components
static double date_hour_from_time_safe(double t) {
	double r = date_hour_from_time(t);
	if (r < 0) r += 24.0;
	return r + 0.0;
}
static double date_min_from_time_safe(double t) {
	double r = date_min_from_time(t);
	if (r < 0) r += 60.0;
	return r + 0.0;
}
static double date_sec_from_time_safe(double t) {
	double r = date_sec_from_time(t);
	if (r < 0) r += 60.0;
	return r + 0.0;
}
static double date_ms_from_time_safe(double t) {
	double r = date_ms_from_time(t);
	if (r < 0) r += 1000.0;
	return r + 0.0;
}

static double date_make_time(double hour, double min, double sec, double ms) {
	if (!isfinite(hour) || !isfinite(min) || !isfinite(sec) || !isfinite(ms)) {
		// Infinity propagates, NaN propagates
		return hour * DATE_MS_PER_HOUR + min * DATE_MS_PER_MINUTE + sec * DATE_MS_PER_SECOND + ms;
	}
	return trunc(hour) * DATE_MS_PER_HOUR + trunc(min) * DATE_MS_PER_MINUTE + trunc(sec) * DATE_MS_PER_SECOND + trunc(ms);
}

static double date_make_day(double year, double month, double date_d) {
	if (!isfinite(year) || !isfinite(month)) return NAN;
	year = trunc(year);
	month = trunc(month);
	if (isfinite(date_d)) date_d = trunc(date_d);
	double y = year + floor(month / 12.0);
	double m = fmod(month, 12.0);
	if (m < 0) m += 12.0;
	int mi = (int)m;
	// Find day number for the start of month m in year y
	int leap = date_days_in_year((int)y) == 366 ? 1 : 0;
	double day_start = date_day_from_year(y) + (double)date_month_start[leap][mi];
	return day_start + date_d - 1.0;
}

static double date_make_date(double day, double time_v) {
	if (!isfinite(day) || !isfinite(time_v)) return day * DATE_MS_PER_DAY + time_v;
	return day * DATE_MS_PER_DAY + time_v;
}

static void date_ensure_tza(void) {
	if (g_date_tza_init) return;
	g_date_tza_init = 1;
	// Compute local timezone offset from system
	time_t now = time(NULL);
	struct tm gmt_tm, local_tm;
	gmtime_r(&now, &gmt_tm);
	localtime_r(&now, &local_tm);
	// Difference in seconds
	time_t gmt_epoch = timegm(&gmt_tm);
	time_t local_epoch = timegm(&local_tm);
	double offset_sec = difftime(local_epoch, gmt_epoch);
	g_date_local_tza_ms = offset_sec * 1000.0;
}

static double date_utc_to_local(double t) {
	date_ensure_tza();
	return t + g_date_local_tza_ms;
}

static double date_local_to_utc(double t) {
	date_ensure_tza();
	return t - g_date_local_tza_ms;
}

// --- Date internal timestamp access ---

static double date_get_time(ASObject* obj) {
	ActionVar* v = getProperty(obj, "__date_time__", 13);
	if (v == NULL) return NAN;
	if (v->type == ACTION_STACK_VALUE_F64) return VAL(double, &v->data.numeric_value);
	if (v->type == ACTION_STACK_VALUE_F32) return (double)VAL(float, &v->data.numeric_value);
	return NAN;
}

// Check if an object has native Date backing (__date_time__ property exists)
static int date_has_backing(void* this_obj) {
	if (this_obj == NULL) return 0;
	// When a Date method is invoked via Function.prototype.call/apply with an
	// ARRAY thisArg (e.g. `Date.prototype.getDate.call([])`), this_obj is a raw
	// ASArray*, NOT an ASObject* — casting it and walking ->properties reads off
	// the end of the array's element buffer (heap-buffer-overflow). An array is
	// never Date-backed, so report "no backing" (Flash returns undefined here).
	if (g_call_this_type == ACTION_STACK_VALUE_ARRAY) return 0;
	ActionVar* v = getProperty((ASObject*)this_obj, "__date_time__", 13);
	return (v != NULL);
}

static void date_set_time(SWFAppContext* ctx, ASObject* obj, double t) {
	ActionVar v = {0};
	v.type = ACTION_STACK_VALUE_F64;
	VAL(double, &v.data.numeric_value) = t;
	setProperty(ctx, obj, "__date_time__", 13, &v);
}

// --- Date.prototype.toString ---

static const char* date_day_names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* date_month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static ActionVar date_return_string(SWFAppContext* app_context, const char* str, u32 len) {
	ActionVar ret = {0};
	ret.type = ACTION_STACK_VALUE_STRING;
	u32 u16_len = 0;
	uint16_t* u16 = utf8_to_u16(app_context, str, len, &u16_len);
	ret.str_size = u16_len;
	ret.data.numeric_value = (u64)u16;
	return ret;
}

static ActionVar builtin_date_toString(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)args; (void)arg_count; (void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);

	if (isnan(t) || isinf(t)) {
		return date_return_string(app_context, "Invalid Date", 12);
	}

	double lt = date_utc_to_local(t);
	double y = date_year_from_time(lt);
	int month = date_month_from_time(lt);
	int day = date_date_from_time(lt);
	int wday = date_week_day(lt);
	int hour = (int)date_hour_from_time_safe(lt);
	int min = (int)date_min_from_time_safe(lt);
	int sec = (int)date_sec_from_time_safe(lt);

	date_ensure_tza();
	int offset_min = (int)(g_date_local_tza_ms / DATE_MS_PER_MINUTE);
	int offset_h = offset_min / 60;
	int offset_m = offset_min % 60;
	if (offset_m < 0) offset_m = -offset_m;

	char buf[80];
	int year_i = (int)y;
	if (year_i < 0) {
		snprintf(buf, sizeof(buf), "%s %s %d %02d:%02d:%02d GMT%+03d%02d %d",
			date_day_names[wday], date_month_names[month], day,
			hour, min, sec, offset_h, offset_m, year_i);
	} else {
		snprintf(buf, sizeof(buf), "%s %s %d %02d:%02d:%02d GMT%+03d%02d %d",
			date_day_names[wday], date_month_names[month], day,
			hour, min, sec, offset_h, offset_m, year_i);
	}

	return date_return_string(app_context, buf, (u32)strlen(buf));
}

// --- Date.prototype.valueOf ---

static ActionVar builtin_date_valueOf(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)app_context; (void)args; (void)arg_count; (void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	double t = date_get_time((ASObject*)this_obj);
	return mathReturnDouble(t);
}

// --- Date getter helpers ---

static double date_arg_to_double(SWFAppContext* app_context, ActionVar* v) {
	if (v->type == ACTION_STACK_VALUE_F64) return VAL(double, &v->data.numeric_value);
	if (v->type == ACTION_STACK_VALUE_F32) return (double)VAL(float, &v->data.numeric_value);
	if (v->type == ACTION_STACK_VALUE_BOOLEAN) return v->data.numeric_value ? 1.0 : 0.0;
	if (v->type == ACTION_STACK_VALUE_UNDEFINED) {
		return (EFFECTIVE_SWF_VERSION() < 7) ? 0.0 : NAN;
	}
	if (v->type == ACTION_STACK_VALUE_NULL) return NAN;
	if (v->type == ACTION_STACK_VALUE_STRING) {
		// Date construction uses strict ECMAScript ToNumber semantics on strings:
		// the entire string must parse as a number, else NaN. (Matches Ruffle's
		// coerce_to_f64 / string_to_f64 path.) `varToDoubleSimple` does lenient
		// strtod, which would let "1234X" → 1234.
		return varToDoubleSWF(app_context, v, EFFECTIVE_SWF_VERSION());
	}
	if (v->type == ACTION_STACK_VALUE_OBJECT) {
		// Call valueOf
		ASObject* o = (ASObject*)(u64)v->data.numeric_value;
		if (o != NULL) {
			ActionVar* vo = getPropertyWithPrototype(o, "valueOf", 7);
			if (vo != NULL && vo->type == ACTION_STACK_VALUE_FUNCTION) {
				ASFunction* func = (ASFunction*)vo->data.numeric_value;
				if (func != NULL && func->advanced_func != NULL) {
					ActionVar result = func->advanced_func(app_context, NULL, 0, NULL, (void*)o);
					return varToDoubleSimple(&result);
				}
			}
		}
		return NAN;
	}
	return NAN;
}

// Flash's integer conversion for date args: NaN → 0, Inf → propagates through arithmetic
static double date_to_integer(double x) {
	if (isnan(x)) return 0.0;
	if (!isfinite(x)) return x;
	return trunc(x);
}

// --- 20 getter methods ---

#define DATE_GETTER(name, expr_local, expr_utc, is_utc) \
static ActionVar builtin_date_##name(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) { \
	(void)app_context; (void)args; (void)arg_count; (void)registers; \
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; } \
	double t = date_get_time((ASObject*)this_obj); \
	if (isnan(t) || !isfinite(t)) return mathReturnDouble(NAN); \
	if (is_utc) { return mathReturnDouble(expr_utc); } \
	else { double lt = date_utc_to_local(t); (void)lt; return mathReturnDouble(expr_local); } \
}

DATE_GETTER(getFullYear, date_year_from_time(lt), date_year_from_time(t), 0)
DATE_GETTER(getYear, date_year_from_time(lt) - 1900.0, date_year_from_time(t) - 1900.0, 0)
DATE_GETTER(getMonth, (double)date_month_from_time(lt), (double)date_month_from_time(t), 0)
DATE_GETTER(getDate, (double)date_date_from_time(lt), (double)date_date_from_time(t), 0)
DATE_GETTER(getDay, (double)date_week_day(lt), (double)date_week_day(t), 0)
DATE_GETTER(getHours, date_hour_from_time_safe(lt), date_hour_from_time_safe(t), 0)
DATE_GETTER(getMinutes, date_min_from_time_safe(lt), date_min_from_time_safe(t), 0)
DATE_GETTER(getSeconds, date_sec_from_time_safe(lt), date_sec_from_time_safe(t), 0)
DATE_GETTER(getMilliseconds, date_ms_from_time_safe(lt), date_ms_from_time_safe(t), 0)
// getTime returns raw UTC timestamp (even Infinity)
static ActionVar builtin_date_getTime(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)app_context; (void)args; (void)arg_count; (void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	double t = date_get_time((ASObject*)this_obj);
	return mathReturnDouble(t);
}
DATE_GETTER(getUTCFullYear, 0, date_year_from_time(t), 1)
DATE_GETTER(getUTCYear, 0, date_year_from_time(t) - 1900.0, 1)
DATE_GETTER(getUTCMonth, 0, (double)date_month_from_time(t), 1)
DATE_GETTER(getUTCDate, 0, (double)date_date_from_time(t), 1)
DATE_GETTER(getUTCDay, 0, (double)date_week_day(t), 1)
DATE_GETTER(getUTCHours, 0, date_hour_from_time_safe(t), 1)
DATE_GETTER(getUTCMinutes, 0, date_min_from_time_safe(t), 1)
DATE_GETTER(getUTCSeconds, 0, date_sec_from_time_safe(t), 1)
DATE_GETTER(getUTCMilliseconds, 0, date_ms_from_time_safe(t), 1)

// getTimezoneOffset: returns -(offset_ms / 60000)
static ActionVar builtin_date_getTimezoneOffset(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)app_context; (void)args; (void)arg_count; (void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	double t = date_get_time((ASObject*)this_obj);
	if (isnan(t) || !isfinite(t)) return mathReturnDouble(NAN);
	date_ensure_tza();
	return mathReturnDouble(-(g_date_local_tza_ms / DATE_MS_PER_MINUTE));
}

// --- 17 setter methods ---
// TimeClip: clamp result to valid range, otherwise NaN
static double date_time_clip(double t) {
	if (!isfinite(t)) return NAN;
	if (fabs(t) > 8.64e15) return NAN;
	return t;
}

// setFullYear(year [, month [, day]])
static ActionVar builtin_date_setFullYear(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double y = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double m = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : (double)date_month_from_time(lt);
	double d = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : (double)date_date_from_time(lt);
	double u = date_time_clip(date_local_to_utc(date_make_date(date_make_day(y, m, d), date_time_within_day(lt))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setMonth(month [, day])
static ActionVar builtin_date_setMonth(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double m = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	if (isnan(m)) m = 0.0;
	double d = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : (double)date_date_from_time(lt);
	double u = date_time_clip(date_local_to_utc(date_make_date(date_make_day(date_year_from_time(lt), m, d), date_time_within_day(lt))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setDate(day)
static ActionVar builtin_date_setDate(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double d = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double u = date_time_clip(date_local_to_utc(date_make_date(date_make_day(date_year_from_time(lt), (double)date_month_from_time(lt), d), date_time_within_day(lt))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setHours(hour [, min [, sec [, ms]]])
static ActionVar builtin_date_setHours(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double h  = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double mi = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : date_min_from_time_safe(lt);
	double s  = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : date_sec_from_time_safe(lt);
	double ms = (arg_count >= 4) ? date_arg_to_double(app_context, &args[3]) : date_ms_from_time_safe(lt);
	double u = date_time_clip(date_local_to_utc(date_make_date(date_day(lt), date_make_time(h, mi, s, ms))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setMinutes(min [, sec [, ms]])
static ActionVar builtin_date_setMinutes(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double mi = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double s  = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : date_sec_from_time_safe(lt);
	double ms = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : date_ms_from_time_safe(lt);
	if (!isfinite(mi)) mi = -2147483648.0;
	if (!isfinite(s))  s  = -2147483648.0;
	if (!isfinite(ms)) ms = -2147483648.0;
	double u = date_time_clip(date_local_to_utc(date_make_date(date_day(lt), date_make_time(date_hour_from_time_safe(lt), mi, s, ms))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setSeconds(sec [, ms])
static ActionVar builtin_date_setSeconds(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double s  = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double ms = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : date_ms_from_time_safe(lt);
	double u = date_time_clip(date_local_to_utc(date_make_date(date_day(lt), date_make_time(date_hour_from_time_safe(lt), date_min_from_time_safe(lt), s, ms))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setMilliseconds(ms)
static ActionVar builtin_date_setMilliseconds(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double ms = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double u = date_time_clip(date_local_to_utc(date_make_date(date_day(lt), date_make_time(date_hour_from_time_safe(lt), date_min_from_time_safe(lt), date_sec_from_time_safe(lt), ms))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setTime(ms) — sets the raw UTC timestamp
static ActionVar builtin_date_setTime(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double ms = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	if (isfinite(ms)) ms = trunc(ms);
	ms = date_time_clip(ms);
	date_set_time(app_context, obj, ms);
	return mathReturnDouble(ms);
}

// setYear(year [, month [, day]]) — legacy: year 0-99 → 1900+year
static ActionVar builtin_date_setYear(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double lt = date_utc_to_local(t);
	double y = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	if (isnan(y) || !isfinite(y)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	y = trunc(y);
	if (y >= 0 && y <= 99) y = 1900.0 + y;
	double m = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : (double)date_month_from_time(lt);
	double d = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : (double)date_date_from_time(lt);
	double u = date_time_clip(date_local_to_utc(date_make_date(date_make_day(y, m, d), date_time_within_day(lt))));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// --- UTC setter variants ---

// setUTCFullYear(year [, month [, day]])
static ActionVar builtin_date_setUTCFullYear(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double y = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double m = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : (double)date_month_from_time(t);
	double d = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : (double)date_date_from_time(t);
	double u = date_time_clip(date_make_date(date_make_day(y, m, d), date_time_within_day(t)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setUTCMonth(month [, day])
static ActionVar builtin_date_setUTCMonth(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double m = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	if (isnan(m)) m = 0.0;
	double d = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : (double)date_date_from_time(t);
	double u = date_time_clip(date_make_date(date_make_day(date_year_from_time(t), m, d), date_time_within_day(t)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setUTCDate(day)
static ActionVar builtin_date_setUTCDate(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double d = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double u = date_time_clip(date_make_date(date_make_day(date_year_from_time(t), (double)date_month_from_time(t), d), date_time_within_day(t)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setUTCHours(hour [, min [, sec [, ms]]])
static ActionVar builtin_date_setUTCHours(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double h  = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double mi = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : date_min_from_time_safe(t);
	double s  = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : date_sec_from_time_safe(t);
	double ms = (arg_count >= 4) ? date_arg_to_double(app_context, &args[3]) : date_ms_from_time_safe(t);
	double u = date_time_clip(date_make_date(date_day(t), date_make_time(h, mi, s, ms)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setUTCMinutes(min [, sec [, ms]])
static ActionVar builtin_date_setUTCMinutes(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double mi = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double s  = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : date_sec_from_time_safe(t);
	double ms = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : date_ms_from_time_safe(t);
	if (!isfinite(mi)) mi = -2147483648.0;
	if (!isfinite(s))  s  = -2147483648.0;
	if (!isfinite(ms)) ms = -2147483648.0;
	double u = date_time_clip(date_make_date(date_day(t), date_make_time(date_hour_from_time_safe(t), mi, s, ms)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setUTCSeconds(sec [, ms])
static ActionVar builtin_date_setUTCSeconds(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double s  = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double ms = (arg_count >= 2) ? date_arg_to_double(app_context, &args[1]) : date_ms_from_time_safe(t);
	double u = date_time_clip(date_make_date(date_day(t), date_make_time(date_hour_from_time_safe(t), date_min_from_time_safe(t), s, ms)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// setUTCMilliseconds(ms)
static ActionVar builtin_date_setUTCMilliseconds(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	if (!date_has_backing(this_obj)) { ActionVar _u = {0}; _u.type = ACTION_STACK_VALUE_UNDEFINED; return _u; }
	ASObject* obj = (ASObject*)this_obj;
	double t = date_get_time(obj);
	if (isnan(t)) { date_set_time(app_context, obj, NAN); return mathReturnDouble(NAN); }
	double ms = (arg_count >= 1) ? date_arg_to_double(app_context, &args[0]) : NAN;
	double u = date_time_clip(date_make_date(date_day(t), date_make_time(date_hour_from_time_safe(t), date_min_from_time_safe(t), date_sec_from_time_safe(t), ms)));
	date_set_time(app_context, obj, u);
	return mathReturnDouble(u);
}

// --- Date.UTC() static method ---

static ActionVar builtin_date_UTC(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers; (void)this_obj;
	// 0-1 args: return undefined
	if (arg_count < 2) {
		ActionVar ret = {0};
		ret.type = ACTION_STACK_VALUE_UNDEFINED;
		return ret;
	}

	double year_d = date_arg_to_double(app_context, &args[0]);
	// Year: C-style int32 cast for NaN/Infinity handling (matches Flash)
	int32_t yi = (int32_t)year_d;
	double year = (double)yi;
	if (yi < 100) year = 1900.0 + year;

	double month = date_arg_to_double(app_context, &args[1]);
	double day   = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : 1.0;
	double hour  = (arg_count >= 4) ? date_arg_to_double(app_context, &args[3]) : 0.0;
	double min_v = (arg_count >= 5) ? date_arg_to_double(app_context, &args[4]) : 0.0;
	double sec   = (arg_count >= 6) ? date_arg_to_double(app_context, &args[5]) : 0.0;
	double ms    = (arg_count >= 7) ? date_arg_to_double(app_context, &args[6]) : 0.0;

	// Month: NaN → NaN (not integer-converted like in setters)
	if (isnan(month)) return mathReturnDouble(NAN);
	month = trunc(month);
	day = trunc(day); hour = trunc(hour); min_v = trunc(min_v); sec = trunc(sec); ms = trunc(ms);

	double d = date_make_day(year, month, day);
	double t = date_make_time(hour, min_v, sec, ms);
	return mathReturnDouble(date_make_date(d, t));
}

// --- Date constructor ---

ASObject* actionDateConstruct(SWFAppContext* app_context, ActionVar* args, u32 arg_count) {
	initDatePrototype(app_context);
	ASObject* date = allocObject(app_context, 4);
	date->native_type = NATIVE_DATE;

	// Set __proto__ to Date.prototype
	ActionVar proto_var = {0};
	proto_var.type = ACTION_STACK_VALUE_OBJECT;
	proto_var.data.numeric_value = (u64)g_date_prototype;
	setProperty(app_context, date, "__proto__", 9, &proto_var);

	// Truncate args at first UNDEFINED (matches Ruffle / ECMAScript semantics:
	// `new Date(undefined)` → current time, same as `new Date()`).
	for (u32 i = 0; i < arg_count; i++) {
		if (args[i].type == ACTION_STACK_VALUE_UNDEFINED) {
			arg_count = i;
			break;
		}
	}

	double t;
	if (arg_count == 0) {
#ifdef MOCK_DATE_TIME
		t = (double)(MOCK_DATE_TIME);
#else
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		t = (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
#endif
	} else if (arg_count == 1) {
		// Single arg: timestamp or valueOf of object
		t = date_arg_to_double(app_context, &args[0]);
	} else {
		// 2-7 args: year, month [, day [, hour [, min [, sec [, ms]]]]]
		double year_d = date_arg_to_double(app_context, &args[0]);
		// Year: C-style int32 cast for NaN/Infinity handling (matches Flash)
		int32_t yi = (int32_t)year_d;
		double year = (double)yi;
		if (yi < 100) year = 1900.0 + year;

		double month = date_arg_to_double(app_context, &args[1]);
		double day   = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : 1.0;
		double hour  = (arg_count >= 4) ? date_arg_to_double(app_context, &args[3]) : 0.0;
		double min_v = (arg_count >= 5) ? date_arg_to_double(app_context, &args[4]) : 0.0;
		double sec   = (arg_count >= 6) ? date_arg_to_double(app_context, &args[5]) : 0.0;
		double ms    = (arg_count >= 7) ? date_arg_to_double(app_context, &args[6]) : 0.0;

		if (isnan(month)) { month = trunc(month); } else { month = trunc(month); }
		day = trunc(day); hour = trunc(hour); min_v = trunc(min_v); sec = trunc(sec); ms = trunc(ms);

		double d = date_make_day(year, month, day);
		double time_v = date_make_time(hour, min_v, sec, ms);
		t = date_local_to_utc(date_make_date(d, time_v));
	}

	date_set_time(app_context, date, t);
	return date;
}

// Date constructor callback (for advanced_func)
// Called when super() targets Date — e.g. from __initializeNative or subclassing.
// If the object already has native backing (native_type != 0), this is a no-op
// (native objects cannot be re-initialized as Date). Otherwise, sets __date_time__
// to make the object behave as a Date.
static ActionVar builtin_date_constructor(SWFAppContext* app_context, ActionVar* args, u32 arg_count, ActionVar* registers, void* this_obj) {
	(void)registers;
	ActionVar ret = {0};
	ret.type = ACTION_STACK_VALUE_UNDEFINED;

	if (this_obj == NULL) return ret;

	ASObject* obj = (ASObject*)this_obj;

	// If object already has native backing from a different type, don't re-initialize
	if (obj->native_type != NATIVE_NONE && obj->native_type != NATIVE_DATE) {
		return ret;
	}

	// Already a Date — don't re-initialize (double-construct protection)
	if (obj->native_type == NATIVE_DATE) {
		return ret;
	}

	// Non-native object: initialize as Date
	obj->native_type = NATIVE_DATE;

	// Truncate args at first UNDEFINED (see actionDateConstruct).
	for (u32 i = 0; i < arg_count; i++) {
		if (args[i].type == ACTION_STACK_VALUE_UNDEFINED) {
			arg_count = i;
			break;
		}
	}

	double t;
	if (arg_count == 0) {
#ifdef MOCK_DATE_TIME
		t = (double)(MOCK_DATE_TIME);
#else
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		t = (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000);
#endif
	} else if (arg_count == 1) {
		t = date_arg_to_double(app_context, &args[0]);
	} else {
		double year_d = date_arg_to_double(app_context, &args[0]);
		int32_t yi = (int32_t)year_d;
		double year = (double)yi;
		if (yi < 100) year = 1900.0 + year;
		double month = date_arg_to_double(app_context, &args[1]);
		double day   = (arg_count >= 3) ? date_arg_to_double(app_context, &args[2]) : 1.0;
		double hour  = (arg_count >= 4) ? date_arg_to_double(app_context, &args[3]) : 0.0;
		double min_v = (arg_count >= 5) ? date_arg_to_double(app_context, &args[4]) : 0.0;
		double sec   = (arg_count >= 6) ? date_arg_to_double(app_context, &args[5]) : 0.0;
		double ms    = (arg_count >= 7) ? date_arg_to_double(app_context, &args[6]) : 0.0;
		month = trunc(month);
		day = trunc(day); hour = trunc(hour); min_v = trunc(min_v); sec = trunc(sec); ms = trunc(ms);
		double d = date_make_day(year, month, day);
		double time_v = date_make_time(hour, min_v, sec, ms);
		t = date_local_to_utc(date_make_date(d, time_v));
	}

	date_set_time(app_context, obj, t);
	return ret;
}

// --- Date prototype initialization ---

void initDatePrototype(SWFAppContext* app_context) {
	if (g_date_init_done) return;
	g_date_init_done = 1;

	memset(&g_date_constructor, 0, sizeof(ASFunction));
	strncpy(g_date_constructor.name, "Date", 255);
	g_date_constructor.function_type = 2;
	g_date_constructor.param_count = 0;
	g_date_constructor.advanced_func = (Function2Ptr)builtin_date_constructor;
	registerNativeFunction(&g_date_constructor);

	g_date_prototype = allocObject(app_context, 48);
	retainObject(g_date_prototype);
	g_date_constructor.prototype_obj = g_date_prototype;

	// Set __proto__ to Object.prototype
	setObjectProto(app_context, g_date_prototype);

	// Register all methods
	struct { const char* name; u32 name_len; Function2Ptr func; } date_methods[] = {
		{"toString",           8, (Function2Ptr)builtin_date_toString},
		{"valueOf",            7, (Function2Ptr)builtin_date_valueOf},
		{"getFullYear",       11, (Function2Ptr)builtin_date_getFullYear},
		{"getYear",            7, (Function2Ptr)builtin_date_getYear},
		{"getMonth",           8, (Function2Ptr)builtin_date_getMonth},
		{"getDate",            7, (Function2Ptr)builtin_date_getDate},
		{"getDay",             6, (Function2Ptr)builtin_date_getDay},
		{"getHours",           8, (Function2Ptr)builtin_date_getHours},
		{"getMinutes",        10, (Function2Ptr)builtin_date_getMinutes},
		{"getSeconds",        10, (Function2Ptr)builtin_date_getSeconds},
		{"getMilliseconds",   15, (Function2Ptr)builtin_date_getMilliseconds},
		{"getTime",            7, (Function2Ptr)builtin_date_getTime},
		{"getTimezoneOffset", 17, (Function2Ptr)builtin_date_getTimezoneOffset},
		{"getUTCFullYear",    14, (Function2Ptr)builtin_date_getUTCFullYear},
		{"getUTCYear",        10, (Function2Ptr)builtin_date_getUTCYear},
		{"getUTCMonth",       11, (Function2Ptr)builtin_date_getUTCMonth},
		{"getUTCDate",        10, (Function2Ptr)builtin_date_getUTCDate},
		{"getUTCDay",          9, (Function2Ptr)builtin_date_getUTCDay},
		{"getUTCHours",       11, (Function2Ptr)builtin_date_getUTCHours},
		{"getUTCMinutes",     13, (Function2Ptr)builtin_date_getUTCMinutes},
		{"getUTCSeconds",     13, (Function2Ptr)builtin_date_getUTCSeconds},
		{"getUTCMilliseconds",18, (Function2Ptr)builtin_date_getUTCMilliseconds},
		{"setFullYear",       11, (Function2Ptr)builtin_date_setFullYear},
		{"setMonth",           8, (Function2Ptr)builtin_date_setMonth},
		{"setDate",            7, (Function2Ptr)builtin_date_setDate},
		{"setHours",           8, (Function2Ptr)builtin_date_setHours},
		{"setMinutes",        10, (Function2Ptr)builtin_date_setMinutes},
		{"setSeconds",        10, (Function2Ptr)builtin_date_setSeconds},
		{"setMilliseconds",   15, (Function2Ptr)builtin_date_setMilliseconds},
		{"setTime",            7, (Function2Ptr)builtin_date_setTime},
		{"setYear",            7, (Function2Ptr)builtin_date_setYear},
		{"setUTCFullYear",    14, (Function2Ptr)builtin_date_setUTCFullYear},
		{"setUTCMonth",       11, (Function2Ptr)builtin_date_setUTCMonth},
		{"setUTCDate",        10, (Function2Ptr)builtin_date_setUTCDate},
		{"setUTCHours",       11, (Function2Ptr)builtin_date_setUTCHours},
		{"setUTCMinutes",     13, (Function2Ptr)builtin_date_setUTCMinutes},
		{"setUTCSeconds",     13, (Function2Ptr)builtin_date_setUTCSeconds},
		{"setUTCMilliseconds",18, (Function2Ptr)builtin_date_setUTCMilliseconds},
	};

	int num_date_methods = 38;
	for (int i = 0; i < num_date_methods; i++) {
		memset(&g_date_funcs[i], 0, sizeof(ASFunction));
		strncpy(g_date_funcs[i].name, date_methods[i].name, 255);
		g_date_funcs[i].function_type = 2;
		g_date_funcs[i].param_count = 0;
		g_date_funcs[i].advanced_func = date_methods[i].func;
		registerNativeFunction(&g_date_funcs[i]);

		ActionVar fv = {0};
		fv.type = ACTION_STACK_VALUE_FUNCTION;
		VAL(u64, &fv.data.numeric_value) = (u64)&g_date_funcs[i];
		setProperty(app_context, g_date_prototype, date_methods[i].name, date_methods[i].name_len, &fv);
	}

	// Register Date.UTC as a static method on the constructor
	{
		memset(&g_date_utc_func, 0, sizeof(ASFunction));
		strncpy(g_date_utc_func.name, "UTC", 255);
		g_date_utc_func.function_type = 2;
		g_date_utc_func.param_count = 0;
		g_date_utc_func.advanced_func = (Function2Ptr)builtin_date_UTC;
		// Mark as native non-constructor: `new Date.UTC()` produces a bare
		// object with no __proto__ (Flash AVM1 quirk). See actionNewMethod.
		g_date_utc_func.no_lazy_prototype = 1;
		registerNativeFunction(&g_date_utc_func);

		ActionVar utc_fv = {0};
		utc_fv.type = ACTION_STACK_VALUE_FUNCTION;
		VAL(u64, &utc_fv.data.numeric_value) = (u64)&g_date_utc_func;

		// Set on the constructor's own_props (accessed as Date.UTC)
		if (g_date_constructor.prototype_obj != NULL) {
			// We need to set UTC on the constructor function itself, not its prototype
			// ASFunction has own_props for this
			if (g_date_constructor.own_props == NULL) {
				g_date_constructor.own_props = allocObject(app_context, 4);
				retainObject(g_date_constructor.own_props);
			}
			setProperty(app_context, g_date_constructor.own_props, "UTC", 3, &utc_fv);
		}
	}
}

// ============================================================================
// Public accessors
// ============================================================================

ActionVar actionDateToString(SWFAppContext* app_context, ASObject* date_obj) {
	return builtin_date_toString(app_context, NULL, 0, NULL, (void*)date_obj);
}

ASFunction* actionDateGetConstructor(SWFAppContext* app_context) {
	initDatePrototype(app_context);
	return &g_date_constructor;
}

// ASnative(103, N) dispatch. Index mapping follows Ruffle's
// core/src/avm1/globals/date.rs method table (with `GET_*` base 0,
// `SET_*` sharing the same base, `GET_UTC_* / SET_UTC_*` = 128 + base,
// CONSTRUCTOR = 256, UTC = 257).
ASFunction* actionDateGetASnativeMethod(SWFAppContext* app_context, u16 index) {
	initDatePrototype(app_context);
	// g_date_funcs layout (registration order in initDatePrototype):
	//  [0] toString            [1] valueOf
	//  [2] getFullYear         [3] getYear
	//  [4] getMonth            [5] getDate
	//  [6] getDay              [7] getHours
	//  [8] getMinutes          [9] getSeconds
	// [10] getMilliseconds    [11] getTime
	// [12] getTimezoneOffset  [13] getUTCFullYear
	// [14] getUTCYear         [15] getUTCMonth
	// [16] getUTCDate         [17] getUTCDay
	// [18] getUTCHours        [19] getUTCMinutes
	// [20] getUTCSeconds      [21] getUTCMilliseconds
	// [22] setFullYear        [23] setMonth
	// [24] setDate            [25] setHours
	// [26] setMinutes         [27] setSeconds
	// [28] setMilliseconds    [29] setTime
	// [30] setYear            [31] setUTCFullYear
	// [32] setUTCMonth        [33] setUTCDate
	// [34] setUTCHours        [35] setUTCMinutes
	// [36] setUTCSeconds      [37] setUTCMilliseconds
	switch (index) {
		case 0:   return &g_date_funcs[2];
		case 1:   return &g_date_funcs[3];
		case 2:   return &g_date_funcs[4];
		case 3:   return &g_date_funcs[5];
		case 4:   return &g_date_funcs[6];
		case 5:   return &g_date_funcs[7];
		case 6:   return &g_date_funcs[8];
		case 7:   return &g_date_funcs[9];
		case 8:   return &g_date_funcs[10];
		case 9:   return &g_date_funcs[22];
		case 10:  return &g_date_funcs[23];
		case 11:  return &g_date_funcs[24];
		case 12:  return &g_date_funcs[25];
		case 13:  return &g_date_funcs[26];
		case 14:  return &g_date_funcs[27];
		case 15:  return &g_date_funcs[28];
		case 16:  return &g_date_funcs[11];
		case 17:  return &g_date_funcs[29];
		case 18:  return &g_date_funcs[12];
		case 19:  return &g_date_funcs[0];
		case 20:  return &g_date_funcs[30];
		case 128: return &g_date_funcs[13];
		case 129: return &g_date_funcs[14];
		case 130: return &g_date_funcs[15];
		case 131: return &g_date_funcs[16];
		case 132: return &g_date_funcs[17];
		case 133: return &g_date_funcs[18];
		case 134: return &g_date_funcs[19];
		case 135: return &g_date_funcs[20];
		case 136: return &g_date_funcs[21];
		case 137: return &g_date_funcs[31];
		case 138: return &g_date_funcs[32];
		case 139: return &g_date_funcs[33];
		case 140: return &g_date_funcs[34];
		case 141: return &g_date_funcs[35];
		case 142: return &g_date_funcs[36];
		case 143: return &g_date_funcs[37];
		case 256: {
			// Initialize bare constructor handle on first use. Shares the
			// same advanced_func as the real Date constructor so `new f()`
			// still initializes the new object as a Date, but has no
			// prototype_obj of its own (matches Ruffle table_native).
			if (g_date_asnative_ctor_func.advanced_func == NULL) {
				memset(&g_date_asnative_ctor_func, 0, sizeof(ASFunction));
				strncpy(g_date_asnative_ctor_func.name, "Date", 255);
				g_date_asnative_ctor_func.function_type = 2;
				g_date_asnative_ctor_func.param_count = 0;
				g_date_asnative_ctor_func.advanced_func = g_date_constructor.advanced_func;
				registerNativeFunction(&g_date_asnative_ctor_func);
			}
			return &g_date_asnative_ctor_func;
		}
		case 257: return &g_date_utc_func;
		default:  return NULL;
	}
}

// Stage 3 collector root marker: Date.prototype + the static constructor's
// lazily-created own_props/prototype_obj.
void dateGcMarkRoots(void)
{
	swfGcMarkObject(g_date_prototype);
	swfGcMarkFunctionPtr(&g_date_constructor);
}
