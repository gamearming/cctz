// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

// This file implements the TimeZoneIf interface using the "zoneinfo"
// data provided by the IANA Time Zone Database (i.e., the only real game
// in town).
//
// TimeZoneInfo represents the history of UTC-offset changes within a time
// zone. Most changes are due to daylight-saving rules, but occasionally
// shifts are made to the time-zone's base offset. The database only attempts
// to be definitive for times since 1970, so be wary of local-time conversions
// before that. Also, rule and zone-boundary changes are made at the whim
// of governments, so the conversion of future times needs to be taken with
// a grain of salt.
//
// For more information see tzfile(5), http://www.iana.org/time-zones, or
// http://en.wikipedia.org/wiki/Zoneinfo.
//
// Note that we assume the proleptic Gregorian calendar and 60-second
// minutes throughout.

#include "time_zone_info.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "civil_time.h"
#include "time_zone_fixed.h"
#include "time_zone_posix.h"

namespace cctz_extension {

namespace {

// A default for cctz_extension::zone_info_source_factory, which simply
// defers to the fallback factory.
std::unique_ptr<cctz::ZoneInfoSource> DefaultFactory(
    const std::string& name,
    const std::function<std::unique_ptr<cctz::ZoneInfoSource>(
        const std::string& name)>& fallback_factory) {
  return fallback_factory(name);
}

}  // namespace

// A "weak" definition for cctz_extension::zone_info_source_factory.
// The user may override this with their own "strong" definition (see
// zone_info_source.h).
#if defined(_MSC_VER)
extern ZoneInfoSourceFactory zone_info_source_factory;
extern ZoneInfoSourceFactory default_factory = DefaultFactory;
#if defined(_M_IX86)
#pragma comment( \
    linker,      \
    "/alternatename:?zone_info_source_factory@cctz_extension@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@3@ABV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@3@@ZA=?default_factory@cctz_extension@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@3@ABV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@ABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@3@@ZA")
#elif defined(_M_IA_64) || defined(_M_AMD64)
#pragma comment( \
    linker,      \
    "/alternatename:?zone_info_source_factory@cctz_extension@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@3@AEBV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@3@@ZEA=?default_factory@cctz_extension@@3P6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@3@AEBV?$function@$$A6A?AV?$unique_ptr@VZoneInfoSource@cctz@@U?$default_delete@VZoneInfoSource@cctz@@@std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@2@@Z@3@@ZEA")
#else
#error Unsupported MSVC platform
#endif  // _MSC_VER
#else
ZoneInfoSourceFactory zone_info_source_factory
    __attribute__((weak)) = DefaultFactory;
#endif

}  // namespace cctz_extension

namespace cctz {

namespace {

inline bool IsLeap(cctz::year_t year) {
  return (year % 4) == 0 && ((year % 100) != 0 || (year % 400) == 0);
}

// The number of days in non-leap and leap years respectively.
const std::int_least32_t kDaysPerYear[2] = {365, 366};

// The day offsets of the beginning of each (1-based) month in non-leap and
// leap years respectively (e.g., 335 days before December in a leap year).
const std::int_least16_t kMonthOffsets[2][1 + 12 + 1] = {
  {-1, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
  {-1, 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

// We reject leap-second encoded zoneinfo and so assume 60-second minutes.
const std::int_least32_t kSecsPerDay = 24 * 60 * 60;

// 400-year chunks always have 146097 days (20871 weeks).
const std::int_least64_t kSecsPer400Years = 146097LL * kSecsPerDay;

// Like kDaysPerYear[] but scaled up by a factor of kSecsPerDay.
const std::int_least32_t kSecsPerYear[2] = {
  365 * kSecsPerDay,
  366 * kSecsPerDay,
};

// Single-byte, unsigned numeric values are encoded directly.
inline std::uint_fast8_t Decode8(const char* cp) {
  return static_cast<std::uint_fast8_t>(*cp) & 0xff;
}

// Multi-byte, numeric values are encoded using a MSB first,
// twos-complement representation. These helpers decode, from
// the given address, 4-byte and 8-byte values respectively.
// Note: If int_fastXX_t == intXX_t and this machine is not
// twos complement, then there will be at least one input value
// we cannot represent.
std::int_fast32_t Decode32(const char* cp) {
  std::uint_fast32_t v = 0;
  for (int i = 0; i != (32 / 8); ++i) v = (v << 8) | Decode8(cp++);
  const std::int_fast32_t s32max = 0x7fffffff;
  const auto s32maxU = static_cast<std::uint_fast32_t>(s32max);
  if (v <= s32maxU) return static_cast<std::int_fast32_t>(v);
  return static_cast<std::int_fast32_t>(v - s32maxU - 1) - s32max - 1;
}

std::int_fast64_t Decode64(const char* cp) {
  std::uint_fast64_t v = 0;
  for (int i = 0; i != (64 / 8); ++i) v = (v << 8) | Decode8(cp++);
  const std::int_fast64_t s64max = 0x7fffffffffffffff;
  const auto s64maxU = static_cast<std::uint_fast64_t>(s64max);
  if (v <= s64maxU) return static_cast<std::int_fast64_t>(v);
  return static_cast<std::int_fast64_t>(v - s64maxU - 1) - s64max - 1;
}

// Generate a year-relative offset for a PosixTransition.
std::int_fast64_t TransOffset(bool leap_year, int jan1_weekday,
                              const PosixTransition& pt) {
  std::int_fast64_t days = 0;
  switch (pt.date.fmt) {
    case PosixTransition::J: {
      days = pt.date.j.day;
      if (!leap_year || days < kMonthOffsets[1][3]) days -= 1;
      break;
    }
    case PosixTransition::N: {
      days = pt.date.n.day;
      break;
    }
    case PosixTransition::M: {
      const bool last_week = (pt.date.m.week == 5);
      days = kMonthOffsets[leap_year][pt.date.m.month + last_week];
      const std::int_fast64_t weekday = (jan1_weekday + days) % 7;
      if (last_week) {
        days -= (weekday + 7 - 1 - pt.date.m.weekday) % 7 + 1;
      } else {
        days += (pt.date.m.weekday + 7 - weekday) % 7;
        days += (pt.date.m.week - 1) * 7;
      }
      break;
    }
  }
  return (days * kSecsPerDay) + pt.time.offset;
}

inline time_zone::civil_lookup MakeUnique(const time_point<sys_seconds>& tp) {
  time_zone::civil_lookup cl;
  cl.kind = time_zone::civil_lookup::UNIQUE;
  cl.pre = cl.trans = cl.post = tp;
  return cl;
}

inline time_zone::civil_lookup MakeUnique(std::int_fast64_t unix_time) {
  return MakeUnique(FromUnixSeconds(unix_time));
}

inline time_zone::civil_lookup MakeSkipped(const Transition& tr,
                                           const civil_second& cs) {
  time_zone::civil_lookup cl;
  cl.kind = time_zone::civil_lookup::SKIPPED;
  cl.pre = FromUnixSeconds(tr.unix_time - 1 + (cs - tr.prev_civil_sec));
  cl.trans = FromUnixSeconds(tr.unix_time);
  cl.post = FromUnixSeconds(tr.unix_time - (tr.civil_sec - cs));
  return cl;
}

inline time_zone::civil_lookup MakeRepeated(const Transition& tr,
                                            const civil_second& cs) {
  time_zone::civil_lookup cl;
  cl.kind = time_zone::civil_lookup::REPEATED;
  cl.pre = FromUnixSeconds(tr.unix_time - 1 - (tr.prev_civil_sec - cs));
  cl.trans = FromUnixSeconds(tr.unix_time);
  cl.post = FromUnixSeconds(tr.unix_time + (cs - tr.civil_sec));
  return cl;
}

inline civil_second YearShift(const civil_second& cs, cctz::year_t shift) {
  return civil_second(cs.year() + shift, cs.month(), cs.day(),
                      cs.hour(), cs.minute(), cs.second());
}

}  // namespace

// What (no leap-seconds) UTC+seconds zoneinfo would look like.
bool TimeZoneInfo::ResetToBuiltinUTC(const sys_seconds& offset) {
  transition_types_.resize(1);
  TransitionType& tt(transition_types_.back());
  tt.utc_offset = static_cast<std::int_least32_t>(offset.count());
  tt.is_dst = false;
  tt.abbr_index = 0;

  transitions_.clear();
  transitions_.reserve(2);
  for (const std::int_fast64_t unix_time : {-(1LL << 59), 2147483647LL}) {
    Transition& tr(*transitions_.emplace(transitions_.end()));
    tr.unix_time = unix_time;
    tr.type_index = 0;
    tr.civil_sec = LocalTime(tr.unix_time, tt).cs;
    tr.prev_civil_sec = tr.civil_sec - 1;
  }

  default_transition_type_ = 0;
  abbreviations_ = FixedOffsetToAbbr(offset);
  abbreviations_.append(1, '\0');  // add NUL
  future_spec_.clear();  // never needed for a fixed-offset zone
  extended_ = false;

  tt.civil_max = LocalTime(sys_seconds::max().count(), tt).cs;
  tt.civil_min = LocalTime(sys_seconds::min().count(), tt).cs;

  transitions_.shrink_to_fit();
  return true;
}

// Builds the in-memory header using the raw bytes from the file.
bool TimeZoneInfo::Header::Build(const tzhead& tzh) {
  std::int_fast32_t v;
  if ((v = Decode32(tzh.tzh_timecnt)) < 0) return false;
  timecnt = static_cast<std::size_t>(v);
  if ((v = Decode32(tzh.tzh_typecnt)) < 0) return false;
  typecnt = static_cast<std::size_t>(v);
  if ((v = Decode32(tzh.tzh_charcnt)) < 0) return false;
  charcnt = static_cast<std::size_t>(v);
  if ((v = Decode32(tzh.tzh_leapcnt)) < 0) return false;
  leapcnt = static_cast<std::size_t>(v);
  if ((v = Decode32(tzh.tzh_ttisstdcnt)) < 0) return false;
  ttisstdcnt = static_cast<std::size_t>(v);
  if ((v = Decode32(tzh.tzh_ttisgmtcnt)) < 0) return false;
  ttisgmtcnt = static_cast<std::size_t>(v);
  return true;
}

// How many bytes of data are associated with this header. The result
// depends upon whether this is a section with 4-byte or 8-byte times.
std::size_t TimeZoneInfo::Header::DataLength(std::size_t time_len) const {
  std::size_t len = 0;
  len += (time_len + 1) * timecnt;  // unix_time + type_index
  len += (4 + 1 + 1) * typecnt;     // utc_offset + is_dst + abbr_index
  len += 1 * charcnt;               // abbreviations
  len += (time_len + 4) * leapcnt;  // leap-time + TAI-UTC
  len += 1 * ttisstdcnt;            // UTC/local indicators
  len += 1 * ttisgmtcnt;            // standard/wall indicators
  return len;
}

// Check that the TransitionType has the expected offset/is_dst/abbreviation.
void TimeZoneInfo::CheckTransition(const std::string& name,
                                   const TransitionType& tt,
                                   std::int_fast32_t offset, bool is_dst,
                                   const std::string& abbr) const {
  if (tt.utc_offset != offset || tt.is_dst != is_dst ||
      &abbreviations_[tt.abbr_index] != abbr) {
    std::clog << name << ": Transition"
              << " offset=" << tt.utc_offset << "/"
              << (tt.is_dst ? "DST" : "STD")
              << "/abbr=" << &abbreviations_[tt.abbr_index]
              << " does not match POSIX spec '" << future_spec_ << "'\n";
  }
}

// zic(8) can generate no-op transitions when a zone changes rules at an
// instant when there is actually no discontinuity.  So we check whether
// two transitions have equivalent types (same offset/is_dst/abbr).
bool TimeZoneInfo::EquivTransitions(std::uint_fast8_t tt1_index,
                                    std::uint_fast8_t tt2_index) const {
  if (tt1_index == tt2_index) return true;
  const TransitionType& tt1(transition_types_[tt1_index]);
  const TransitionType& tt2(transition_types_[tt2_index]);
  if (tt1.is_dst != tt2.is_dst) return false;
  if (tt1.utc_offset != tt2.utc_offset) return false;
  if (tt1.abbr_index != tt2.abbr_index) return false;
  return true;
}

// Use the POSIX-TZ-environment-variable-style string to handle times
// in years after the last transition stored in the zoneinfo data.
void TimeZoneInfo::ExtendTransitions(const std::string& name,
                                     const Header& hdr) {
  extended_ = false;
  bool extending = !future_spec_.empty();

  PosixTimeZone posix;
  if (extending && !ParsePosixSpec(future_spec_, &posix)) {
    std::clog << name << ": Failed to parse '" << future_spec_ << "'\n";
    extending = false;
  }

  if (extending && posix.dst_abbr.empty()) {  // std only
    // The future specification should match the last/default transition,
    // and that means that handling the future will fall out naturally.
    std::uint_fast8_t index = default_transition_type_;
    if (hdr.timecnt != 0) index = transitions_[hdr.timecnt - 1].type_index;
    const TransitionType& tt(transition_types_[index]);
    CheckTransition(name, tt, posix.std_offset, false, posix.std_abbr);
    extending = false;
  }

  if (extending && hdr.timecnt < 2) {
    std::clog << name << ": Too few transitions for POSIX spec\n";
    extending = false;
  }

  if (!extending) {
    // Ensure that there is always a transition in the second half of the
    // time line (the BIG_BANG transition is in the first half) so that the
    // signed difference between a civil_second and the civil_second of its
    // previous transition is always representable, without overflow.
    const Transition& last(transitions_.back());
    if (last.unix_time < 0) {
      const std::uint_fast8_t type_index = last.type_index;
      Transition& tr(*transitions_.emplace(transitions_.end()));
      tr.unix_time = 2147483647;  // 2038-01-19T03:14:07+00:00
      tr.type_index = type_index;
    }
    return;  // last transition wins
  }

  // Extend the transitions for an additional 400 years using the
  // future specification. Years beyond those can be handled by
  // mapping back to a cycle-equivalent year within that range.
  // zic(8) should probably do this so that we don't have to.
  // TODO: Reduce the extension by the number of compatible
  // transitions already in place.
  transitions_.reserve(hdr.timecnt + 400 * 2 + 1);
  transitions_.resize(hdr.timecnt + 400 * 2);
  extended_ = true;

  // The future specification should match the last two transitions,
  // and those transitions should have different is_dst flags.
  const Transition* tr0 = &transitions_[hdr.timecnt - 1];
  const Transition* tr1 = &transitions_[hdr.timecnt - 2];
  const TransitionType* tt0 = &transition_types_[tr0->type_index];
  const TransitionType* tt1 = &transition_types_[tr1->type_index];
  const TransitionType& spring(tt0->is_dst ? *tt0 : *tt1);
  const TransitionType& autumn(tt0->is_dst ? *tt1 : *tt0);
  CheckTransition(name, spring, posix.dst_offset, true, posix.dst_abbr);
  CheckTransition(name, autumn, posix.std_offset, false, posix.std_abbr);

  // Add the transitions to tr1 and back to tr0 for each extra year.
  last_year_ = LocalTime(tr0->unix_time, *tt0).cs.year();
  bool leap_year = IsLeap(last_year_);
  const civil_day jan1(last_year_, 1, 1);
  std::int_fast64_t jan1_time = civil_second(jan1) - civil_second();
  int jan1_weekday = (static_cast<int>(get_weekday(jan1)) + 1) % 7;
  Transition* tr = &transitions_[hdr.timecnt];  // next trans to fill
  if (LocalTime(tr1->unix_time, *tt1).cs.year() != last_year_) {
    // Add a single extra transition to align to a calendar year.
    transitions_.resize(transitions_.size() + 1);
    assert(tr == &transitions_[hdr.timecnt]);  // no reallocation
    const PosixTransition& pt1(tt0->is_dst ? posix.dst_end : posix.dst_start);
    std::int_fast64_t tr1_offset = TransOffset(leap_year, jan1_weekday, pt1);
    tr->unix_time = jan1_time + tr1_offset - tt0->utc_offset;
    tr++->type_index = tr1->type_index;
    tr0 = &transitions_[hdr.timecnt];
    tr1 = &transitions_[hdr.timecnt - 1];
    tt0 = &transition_types_[tr0->type_index];
    tt1 = &transition_types_[tr1->type_index];
  }
  const PosixTransition& pt1(tt0->is_dst ? posix.dst_end : posix.dst_start);
  const PosixTransition& pt0(tt0->is_dst ? posix.dst_start : posix.dst_end);
  for (const cctz::year_t limit = last_year_ + 400; last_year_ < limit;) {
    last_year_ += 1;  // an additional year of generated transitions
    jan1_time += kSecsPerYear[leap_year];
    jan1_weekday = (jan1_weekday + kDaysPerYear[leap_year]) % 7;
    leap_year = !leap_year && IsLeap(last_year_);
    std::int_fast64_t tr1_offset = TransOffset(leap_year, jan1_weekday, pt1);
    tr->unix_time = jan1_time + tr1_offset - tt0->utc_offset;
    tr++->type_index = tr1->type_index;
    std::int_fast64_t tr0_offset = TransOffset(leap_year, jan1_weekday, pt0);
    tr->unix_time = jan1_time + tr0_offset - tt1->utc_offset;
    tr++->type_index = tr0->type_index;
  }
  assert(tr == &transitions_[0] + transitions_.size());
}

bool TimeZoneInfo::Load(const std::string& name, ZoneInfoSource* zip) {
  // Read and validate the header.
  tzhead tzh;
  if (zip->Read(&tzh, sizeof(tzh)) != sizeof(tzh))
    return false;
  if (strncmp(tzh.tzh_magic, TZ_MAGIC, sizeof(tzh.tzh_magic)) != 0)
    return false;
  Header hdr;
  if (!hdr.Build(tzh))
    return false;
  std::size_t time_len = 4;
  if (tzh.tzh_version[0] != '\0') {
    // Skip the 4-byte data.
    if (zip->Skip(hdr.DataLength(time_len)) != 0)
      return false;
    // Read and validate the header for the 8-byte data.
    if (zip->Read(&tzh, sizeof(tzh)) != sizeof(tzh))
      return false;
    if (strncmp(tzh.tzh_magic, TZ_MAGIC, sizeof(tzh.tzh_magic)) != 0)
      return false;
    if (tzh.tzh_version[0] == '\0')
      return false;
    if (!hdr.Build(tzh))
      return false;
    time_len = 8;
  }
  if (hdr.typecnt == 0)
    return false;
  if (hdr.leapcnt != 0) {
    // This code assumes 60-second minutes so we do not want
    // the leap-second encoded zoneinfo. We could reverse the
    // compensation, but the "right" encoding is rarely used
    // so currently we simply reject such data.
    return false;
  }
  if (hdr.ttisstdcnt != 0 && hdr.ttisstdcnt != hdr.typecnt)
    return false;
  if (hdr.ttisgmtcnt != 0 && hdr.ttisgmtcnt != hdr.typecnt)
    return false;

  // Read the data into a local buffer.
  std::size_t len = hdr.DataLength(time_len);
  std::vector<char> tbuf(len);
  if (zip->Read(tbuf.data(), len) != len)
    return false;
  const char* bp = tbuf.data();

  // Decode and validate the transitions.
  transitions_.reserve(hdr.timecnt + 2);  // We might add a couple.
  transitions_.resize(hdr.timecnt);
  for (std::size_t i = 0; i != hdr.timecnt; ++i) {
    transitions_[i].unix_time = (time_len == 4) ? Decode32(bp) : Decode64(bp);
    bp += time_len;
    if (i != 0) {
      // Check that the transitions are ordered by time (as zic guarantees).
      if (!Transition::ByUnixTime()(transitions_[i - 1], transitions_[i]))
        return false;  // out of order
    }
  }
  bool seen_type_0 = false;
  for (std::size_t i = 0; i != hdr.timecnt; ++i) {
    transitions_[i].type_index = Decode8(bp++);
    if (transitions_[i].type_index >= hdr.typecnt)
      return false;
    if (transitions_[i].type_index == 0)
      seen_type_0 = true;
  }

  // Decode and validate the transition types.
  transition_types_.resize(hdr.typecnt);
  for (std::size_t i = 0; i != hdr.typecnt; ++i) {
    transition_types_[i].utc_offset =
        static_cast<std::int_least32_t>(Decode32(bp));
    if (transition_types_[i].utc_offset >= kSecsPerDay ||
        transition_types_[i].utc_offset <= -kSecsPerDay)
      return false;
    bp += 4;
    transition_types_[i].is_dst = (Decode8(bp++) != 0);
    transition_types_[i].abbr_index = Decode8(bp++);
    if (transition_types_[i].abbr_index >= hdr.charcnt)
      return false;
  }

  // Determine the before-first-transition type.
  default_transition_type_ = 0;
  if (seen_type_0 && hdr.timecnt != 0) {
    std::uint_fast8_t index = 0;
    if (transition_types_[0].is_dst) {
      index = transitions_[0].type_index;
      while (index != 0 && transition_types_[index].is_dst)
        --index;
    }
    while (index != hdr.typecnt && transition_types_[index].is_dst)
      ++index;
    if (index != hdr.typecnt)
      default_transition_type_ = index;
  }

  // Copy all the abbreviations.
  abbreviations_.assign(bp, hdr.charcnt);
  bp += hdr.charcnt;

  // Skip the unused portions. We've already dispensed with leap-second
  // encoded zoneinfo. The ttisstd/ttisgmt indicators only apply when
  // interpreting a POSIX spec that does not include start/end rules, and
  // that isn't the case here (see "zic -p").
  bp += (8 + 4) * hdr.leapcnt;  // leap-time + TAI-UTC
  bp += 1 * hdr.ttisstdcnt;     // UTC/local indicators
  bp += 1 * hdr.ttisgmtcnt;     // standard/wall indicators
  assert(bp == tbuf.data() + tbuf.size());

  future_spec_.clear();
  if (tzh.tzh_version[0] != '\0') {
    // Snarf up the NL-enclosed future POSIX spec. Note
    // that version '3' files utilize an extended format.
    auto get_char = [](ZoneInfoSource* zip) -> int {
      unsigned char ch;  // all non-EOF results are positive
      return (zip->Read(&ch, 1) == 1) ? ch : EOF;
    };
    if (get_char(zip) != '\n')
      return false;
    for (int c = get_char(zip); c != '\n'; c = get_char(zip)) {
      if (c == EOF)
        return false;
      future_spec_.push_back(static_cast<char>(c));
    }
  }

  // We don't check for EOF so that we're forwards compatible.

  // Trim redundant transitions. zic may have added these to work around
  // differences between the glibc and reference implementations (see
  // zic.c:dontmerge) and the Qt library (see zic.c:WORK_AROUND_QTBUG_53071).
  // For us, they just get in the way when we do future_spec_ extension.
  while (hdr.timecnt > 1) {
    if (!EquivTransitions(transitions_[hdr.timecnt - 1].type_index,
                          transitions_[hdr.timecnt - 2].type_index)) {
      break;
    }
    hdr.timecnt -= 1;
  }
  transitions_.resize(hdr.timecnt);

  // Ensure that there is always a transition in the first half of the
  // time line (the second half is handled in ExtendTransitions()) so that
  // the signed difference between a civil_second and the civil_second of
  // its previous transition is always representable, without overflow.
  // A contemporary zic will usually have already done this for us.
  if (transitions_.empty() || transitions_.front().unix_time >= 0) {
    Transition& tr(*transitions_.emplace(transitions_.begin()));
    tr.unix_time = -(1LL << 59);  // see tz/zic.c "BIG_BANG"
    tr.type_index = default_transition_type_;
    hdr.timecnt += 1;
  }

  // Extend the transitions using the future specification.
  ExtendTransitions(name, hdr);

  // Compute the local civil time for each transition and the preceeding
  // second. These will be used for reverse conversions in MakeTime().
  const TransitionType* ttp = &transition_types_[default_transition_type_];
  for (std::size_t i = 0; i != transitions_.size(); ++i) {
    Transition& tr(transitions_[i]);
    tr.prev_civil_sec = LocalTime(tr.unix_time, *ttp).cs - 1;
    ttp = &transition_types_[tr.type_index];
    tr.civil_sec = LocalTime(tr.unix_time, *ttp).cs;
    if (i != 0) {
      // Check that the transitions are ordered by civil time. Essentially
      // this means that an offset change cannot cross another such change.
      // No one does this in practice, and we depend on it in MakeTime().
      if (!Transition::ByCivilTime()(transitions_[i - 1], tr))
        return false;  // out of order
    }
  }

  // Compute the maximum/minimum civil times that can be converted to a
  // time_point<sys_seconds> for each of the zone's transition types.
  for (auto& tt : transition_types_) {
    tt.civil_max = LocalTime(sys_seconds::max().count(), tt).cs;
    tt.civil_min = LocalTime(sys_seconds::min().count(), tt).cs;
  }

  transitions_.shrink_to_fit();
  return true;
}

namespace {

// A stdio(3)-backed implementation of ZoneInfoSource.
class FileZoneInfoSource : public ZoneInfoSource {
 public:
  static std::unique_ptr<ZoneInfoSource> Open(const std::string& name);

  std::size_t Read(void* ptr, std::size_t size) override {
    return fread(ptr, 1, size, fp_);
  }
  int Skip(std::size_t offset) override {
    return fseek(fp_, static_cast<long>(offset), SEEK_CUR);
  }

 private:
  explicit FileZoneInfoSource(FILE* fp) : fp_(fp) {}
  ~FileZoneInfoSource() { fclose(fp_); }

  FILE* const fp_;
};

std::unique_ptr<ZoneInfoSource> FileZoneInfoSource::Open(
    const std::string& name) {
  // Use of the "file:" prefix is intended for testing purposes only.
  if (name.compare(0, 5, "file:") == 0) return Open(name.substr(5));

  // Map the time-zone name to a path name.
  std::string path;
  if (name.empty() || name[0] != '/') {
    const char* tzdir = "/usr/share/zoneinfo";
    char* tzdir_env = nullptr;
#if defined(_MSC_VER)
    _dupenv_s(&tzdir_env, nullptr, "TZDIR");
#else
    tzdir_env = std::getenv("TZDIR");
#endif
    if (tzdir_env && *tzdir_env) tzdir = tzdir_env;
    path += tzdir;
    path += '/';
#if defined(_MSC_VER)
    free(tzdir_env);
#endif
  }
  path += name;

  // Open the zoneinfo file.
#if defined(_MSC_VER)
  FILE* fp;
  if (fopen_s(&fp, path.c_str(), "rb") != 0) fp = nullptr;
#else
  FILE* fp = fopen(path.c_str(), "rb");
#endif
  if (fp == nullptr) return nullptr;
  return std::unique_ptr<ZoneInfoSource>(new FileZoneInfoSource(fp));
}

}  // namespace

bool TimeZoneInfo::Load(const std::string& name) {
  // We can ensure that the loading of UTC or any other fixed-offset
  // zone never fails because the simple, fixed-offset state can be
  // internally generated. Note that this depends on our choice to not
  // accept leap-second encoded ("right") zoneinfo.
  auto offset = sys_seconds::zero();
  if (FixedOffsetFromName(name, &offset)) {
    return ResetToBuiltinUTC(offset);
  }

  // Find and use a ZoneInfoSource to load the named zone.
  auto zip = cctz_extension::zone_info_source_factory(
      name, [](const std::string& name) {
        return FileZoneInfoSource::Open(name);  // fallback factory
      });
  return zip != nullptr && Load(name, zip.get());
}

// BreakTime() translation for a particular transition type.
time_zone::absolute_lookup TimeZoneInfo::LocalTime(
    std::int_fast64_t unix_time, const TransitionType& tt) const {
  time_zone::absolute_lookup al;

  // A civil time in "+offset" looks like (time+offset) in UTC.
  // Note: We perform two additions in the civil_second domain to
  // sidestep the chance of overflow in (unix_time + tt.utc_offset).
  al.cs = (civil_second() + unix_time) + tt.utc_offset;

  // Handle offset, is_dst, and abbreviation.
  al.offset = tt.utc_offset;
  al.is_dst = tt.is_dst;
  al.abbr = &abbreviations_[tt.abbr_index];

  return al;
}

// MakeTime() translation with a conversion-preserving +N * 400-year shift.
time_zone::civil_lookup TimeZoneInfo::TimeLocal(const civil_second& cs,
                                                cctz::year_t c4_shift) const {
  assert(last_year_ - 400 < cs.year() && cs.year() <= last_year_);
  time_zone::civil_lookup cl = MakeTime(cs);
  if (c4_shift > sys_seconds::max().count() / kSecsPer400Years) {
    cl.pre = cl.trans = cl.post = time_point<sys_seconds>::max();
  } else {
    const auto offset = sys_seconds(c4_shift * kSecsPer400Years);
    const auto limit = time_point<sys_seconds>::max() - offset;
    for (auto* tp : {&cl.pre, &cl.trans, &cl.post}) {
      if (*tp > limit) {
        *tp = time_point<sys_seconds>::max();
      } else {
        *tp += offset;
      }
    }
  }
  return cl;
}

time_zone::absolute_lookup TimeZoneInfo::BreakTime(
    const time_point<sys_seconds>& tp) const {
  std::int_fast64_t unix_time = ToUnixSeconds(tp);
  const std::size_t timecnt = transitions_.size();
  if (timecnt == 0 || unix_time < transitions_[0].unix_time) {
    const std::uint_fast8_t type_index = default_transition_type_;
    return LocalTime(unix_time, transition_types_[type_index]);
  }
  if (unix_time >= transitions_[timecnt - 1].unix_time) {
    // After the last transition. If we extended the transitions using
    // future_spec_, shift back to a supported year using the 400-year
    // cycle of calendaric equivalence and then compensate accordingly.
    if (extended_) {
      const std::int_fast64_t diff =
          unix_time - transitions_[timecnt - 1].unix_time;
      const cctz::year_t shift = diff / kSecsPer400Years + 1;
      const auto d = sys_seconds(shift * kSecsPer400Years);
      time_zone::absolute_lookup al = BreakTime(tp - d);
      al.cs = YearShift(al.cs, shift * 400);
      return al;
    }
    const std::uint_fast8_t type_index = transitions_[timecnt - 1].type_index;
    return LocalTime(unix_time, transition_types_[type_index]);
  }

  const std::size_t hint = local_time_hint_.load(std::memory_order_relaxed);
  if (0 < hint && hint < timecnt) {
    if (unix_time < transitions_[hint].unix_time) {
      if (!(unix_time < transitions_[hint - 1].unix_time)) {
        const std::uint_fast8_t type_index = transitions_[hint - 1].type_index;
        return LocalTime(unix_time, transition_types_[type_index]);
      }
    }
  }

  const Transition target = {unix_time, 0, civil_second(), civil_second()};
  const Transition* begin = &transitions_[0];
  const Transition* tr = std::upper_bound(begin, begin + timecnt, target,
                                          Transition::ByUnixTime());
  local_time_hint_.store(static_cast<std::size_t>(tr - begin),
                         std::memory_order_relaxed);
  const std::uint_fast8_t type_index = (--tr)->type_index;
  return LocalTime(unix_time, transition_types_[type_index]);
}

time_zone::civil_lookup TimeZoneInfo::MakeTime(const civil_second& cs) const {
  const std::size_t timecnt = transitions_.size();
  assert(timecnt != 0);  // We always add a transition.

  // Find the first transition after our target civil time.
  const Transition* tr = nullptr;
  const Transition* begin = &transitions_[0];
  const Transition* end = begin + timecnt;
  if (cs < begin->civil_sec) {
    tr = begin;
  } else if (!(cs < transitions_[timecnt - 1].civil_sec)) {
    tr = end;
  } else {
    const std::size_t hint = time_local_hint_.load(std::memory_order_relaxed);
    if (0 < hint && hint < timecnt) {
      if (cs < transitions_[hint].civil_sec) {
        if (!(cs < transitions_[hint - 1].civil_sec)) {
          tr = begin + hint;
        }
      }
    }
    if (tr == nullptr) {
      const Transition target = {0, 0, cs, civil_second()};
      tr = std::upper_bound(begin, end, target, Transition::ByCivilTime());
      time_local_hint_.store(static_cast<std::size_t>(tr - begin),
                             std::memory_order_relaxed);
    }
  }

  if (tr == begin) {
    if (!(tr->prev_civil_sec < cs)) {
      // Before first transition, so use the default offset.
      const TransitionType& tt(transition_types_[default_transition_type_]);
      if (cs < tt.civil_min) return MakeUnique(time_point<sys_seconds>::min());
      return MakeUnique(cs - (civil_second() + tt.utc_offset));
    }
    // tr->prev_civil_sec < cs < tr->civil_sec
    return MakeSkipped(*tr, cs);
  }

  if (tr == end) {
    if ((--tr)->prev_civil_sec < cs) {
      // After the last transition. If we extended the transitions using
      // future_spec_, shift back to a supported year using the 400-year
      // cycle of calendaric equivalence and then compensate accordingly.
      if (extended_ && cs.year() > last_year_) {
        const cctz::year_t shift = (cs.year() - last_year_ - 1) / 400 + 1;
        return TimeLocal(YearShift(cs, shift * -400), shift);
      }
      const TransitionType& tt(transition_types_[tr->type_index]);
      if (cs > tt.civil_max) return MakeUnique(time_point<sys_seconds>::max());
      return MakeUnique(tr->unix_time + (cs - tr->civil_sec));
    }
    // tr->civil_sec <= cs <= tr->prev_civil_sec
    return MakeRepeated(*tr, cs);
  }

  if (tr->prev_civil_sec < cs) {
    // tr->prev_civil_sec < cs < tr->civil_sec
    return MakeSkipped(*tr, cs);
  }

  if (!((--tr)->prev_civil_sec < cs)) {
    // tr->civil_sec <= cs <= tr->prev_civil_sec
    return MakeRepeated(*tr, cs);
  }

  // In between transitions.
  return MakeUnique(tr->unix_time + (cs - tr->civil_sec));
}

}  // namespace cctz
