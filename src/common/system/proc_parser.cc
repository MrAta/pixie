#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/strings/numbers.h>
#include <absl/strings/substitute.h>

#include "src/common/fs/fs_wrapper.h"
#include "src/common/system/proc_parser.h"

namespace pl {
namespace system {

/**
 * These constants are used to ignore virtual and local network interfaces.
 */
const std::vector<std::string> kNetIFaceIgnorePrefix = {
    "v",
    "docker",
    "lo",
};

/*************************************************
 * Constants for the /proc/stat file
 *************************************************/
constexpr int kProcStatCPUNumFields = 11;
constexpr int KProcStatCPUUTimeField = 1;
constexpr int KProcStatCPUKTimeField = 3;

/*************************************************
 * Constants for the /proc/<pid>/net/dev file
 *************************************************/
constexpr int kProcNetDevNumFields = 17;

constexpr int kProcNetDevIFaceField = 0;
constexpr int kProcNetDevRxBytesField = 1;
constexpr int kProcNetDevRxPacketsField = 2;
constexpr int kProcNetDevRxErrsField = 3;
constexpr int kProcNetDevRxDropField = 4;

constexpr int kProcNetDevTxBytesField = 9;
constexpr int kProcNetDevTxPacketsField = 10;
constexpr int kProcNetDevTxErrsField = 11;
constexpr int kProcNetDevTxDropField = 12;

/*************************************************
 * constexprants for the /proc/<pid>/stat file
 *************************************************/
constexpr int kProcStatNumFields = 52;

constexpr int kProcStatPIDField = 0;
constexpr int kProcStatProcessNameField = 1;

constexpr int kProcStatMinorFaultsField = 9;
constexpr int kProcStatMajorFaultsField = 11;

constexpr int kProcStatUTimeField = 13;
constexpr int kProcStatKTimeField = 14;
constexpr int kProcStatNumThreadsField = 19;

constexpr int kProcStatStartTimeField = 21;

constexpr int kProcStatVSizeField = 22;
constexpr int kProcStatRSSField = 23;

ProcParser::ProcParser(const system::Config& cfg) {
  CHECK(cfg.HasConfig()) << "System config is required for the ProcParser";
  ns_per_kernel_tick_ = static_cast<int64_t>(1E9 / cfg.KernelTicksPerSecond());
  clock_realtime_offset_ = cfg.ClockRealTimeOffset();
  bytes_per_page_ = cfg.PageSize();
  proc_base_path_ = cfg.proc_path();
}

Status ProcParser::ParseNetworkStatAccumulateIFaceData(
    const std::vector<std::string_view>& dev_stat_record, NetworkStats* out) {
  DCHECK(out != nullptr);

  int64_t val;
  bool ok = true;
  // Rx Data.
  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevRxBytesField], &val);
  out->rx_bytes += val;

  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevRxPacketsField], &val);
  out->rx_packets += val;

  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevRxDropField], &val);
  out->rx_drops += val;

  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevRxErrsField], &val);
  out->rx_errs += val;

  // Tx Data.
  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevTxBytesField], &val);
  out->tx_bytes += val;

  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevTxPacketsField], &val);
  out->tx_packets += val;

  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevTxDropField], &val);
  out->tx_drops += val;

  ok &= absl::SimpleAtoi(dev_stat_record[kProcNetDevTxErrsField], &val);
  out->tx_errs += val;

  if (!ok) {
    // This should never happen since it requires the file to be ill-formed
    // by the kernel.
    return error::Internal("failed to parse net dev file");
  }

  return Status::OK();
}

bool ShouldSkipNetIFace(const std::string_view iface) {
  // TODO(zasgar): We might want to make this configurable at some point.
  for (const auto& prefix : kNetIFaceIgnorePrefix) {
    if (absl::StartsWith(iface, prefix)) {
      return true;
    }
  }
  return false;
}

Status ProcParser::ParseProcPIDNetDev(int32_t pid, NetworkStats* out) const {
  /**
   * Sample file:
   * Inter-|   Receive                                                | Transmit
   * face |bytes    packets errs drop fifo frame compressed multicast|bytes
   * packets errs drop fifo colls carrier compressed ens33: 54504114   65296 0
   * 0    0     0          0         0 4258632   39739    0    0    0     0 0 0
   * vnet1: 3936114   23029    0    0    0 0          0         0 551949355
   * 42771    0    0    0     0       0          0
   *
   */
  DCHECK(out != nullptr);

  std::string fpath = absl::Substitute("$0/$1/net/dev", proc_base_path_, pid);
  std::ifstream ifs;
  ifs.open(fpath);
  if (!ifs) {
    return error::Internal("Failed to open file $0", fpath);
  }

  // Ignore the first two lines since they are just headers;
  const int kHeaderLines = 2;
  for (int i = 0; i < kHeaderLines; ++i) {
    ifs.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }

  std::string line;
  while (std::getline(ifs, line)) {
    std::vector<std::string_view> split = absl::StrSplit(line, " ", absl::SkipWhitespace());
    // We check less than in case more fields are added later.
    if (split.size() < kProcNetDevNumFields) {
      return error::Internal("failed to parse net dev file, incorrect number of fields");
    }

    if (ShouldSkipNetIFace(split[kProcNetDevIFaceField])) {
      continue;
    }

    // We should track this interface. Accumulate the results.
    auto s = ParseNetworkStatAccumulateIFaceData(split, out);
    if (!s.ok()) {
      // Empty out the stats so we don't leave intermediate results.
      return s;
    }
  }

  return Status::OK();
}

Status ProcParser::ParseProcPIDStat(int32_t pid, ProcessStats* out) const {
  /**
   * Sample file:
   * 4602 (ibazel) S 3260 4602 3260 34818 4602 1077936128 1799 174589 \
   * 55 68 8 23 106 72 20 0 13 0 14329 114384896 2577 18446744073709551615 \
   * 4194304 7917379 140730842479232 0 0 0 1006254592 0 2143420159 0 0 0 17 \
   * 3 0 0 3 0 0 12193792 12432192 34951168 140730842488151 140730842488200 \
   * 140730842488200 140730842492896 0
   */
  DCHECK(out != nullptr);
  std::string fpath = absl::Substitute("$0/$1/stat", proc_base_path_, pid);

  std::ifstream ifs;
  ifs.open(fpath);
  if (!ifs) {
    return error::Internal("Failed to open file $0", fpath);
  }

  std::string line;
  bool ok = true;
  if (std::getline(ifs, line)) {
    std::vector<std::string_view> split = absl::StrSplit(line, " ", absl::SkipWhitespace());
    // We check less than in case more fields are added later.
    if (split.size() < kProcStatNumFields) {
      return error::Unknown("Incorrect number of fields in stat file: $0", fpath);
    }
    ok &= absl::SimpleAtoi(split[kProcStatPIDField], &out->pid);
    // The name is surrounded by () we remove it here.
    const std::string_view& name_field = split[kProcStatProcessNameField];
    if (name_field.length() > 2) {
      out->process_name = std::string(name_field.substr(1, name_field.size() - 2));
    } else {
      ok = false;
    }
    ok &= absl::SimpleAtoi(split[kProcStatMinorFaultsField], &out->minor_faults);
    ok &= absl::SimpleAtoi(split[kProcStatMajorFaultsField], &out->major_faults);

    ok &= absl::SimpleAtoi(split[kProcStatUTimeField], &out->utime_ns);
    ok &= absl::SimpleAtoi(split[kProcStatKTimeField], &out->ktime_ns);
    // The kernel tracks utime and ktime in kernel ticks.
    out->utime_ns *= ns_per_kernel_tick_;
    out->ktime_ns *= ns_per_kernel_tick_;

    ok &= absl::SimpleAtoi(split[kProcStatNumThreadsField], &out->num_threads);
    ok &= absl::SimpleAtoi(split[kProcStatVSizeField], &out->vsize_bytes);
    ok &= absl::SimpleAtoi(std::string(split[kProcStatRSSField]), &out->rss_bytes);

    // RSS is in pages.
    out->rss_bytes *= bytes_per_page_;

  } else {
    return error::Internal("Failed to read proc stat file: $0", fpath);
  }

  if (!ok) {
    // This should never happen since it requires the file to be ill-formed
    // by the kernel.
    return error::Internal("failed to parse stat file ($0). ATOI failed.", fpath);
  }
  return Status::OK();
}

Status ProcParser::ParseProcPIDStatIO(int32_t pid, ProcessStats* out) const {
  /**
   * Sample file:
   *   rchar: 5405203
   *   wchar: 1239158
   *   syscr: 10608
   *   syscw: 3141
   *   read_bytes: 17838080
   *   write_bytes: 634880
   *   cancelled_write_bytes: 192512
   */
  DCHECK(out != nullptr);
  std::string fpath = absl::Substitute("$0/$1/io", proc_base_path_, pid);

  // Just to be safe when using offsetof, make sure object is standard layout.
  static_assert(std::is_standard_layout<ProcessStats>::value);

  static absl::flat_hash_map<std::string_view, size_t> field_name_to_offset_map{
      {"rchar:", offsetof(ProcessStats, rchar_bytes)},
      {"wchar:", offsetof(ProcessStats, wchar_bytes)},
      {"read_bytes:", offsetof(ProcessStats, read_bytes)},
      {"write_bytes:", offsetof(ProcessStats, write_bytes)},
  };

  return ParseFromKeyValueFile(fpath, field_name_to_offset_map, reinterpret_cast<uint8_t*>(out),
                               1 /*field_value_multipler*/);
}

Status ProcParser::ParseProcStat(SystemStats* out) const {
  /**
   * Sample file:
   * cpu  248758 4995 78314 12965346 10040 0 5498 0 0 0
   * cpu0 43574 817 13011 2159486 994 0 1022 0 0 0
   * ...
   */
  CHECK(out != nullptr);
  std::string fpath = absl::Substitute("$0/stat", proc_base_path_);
  std::ifstream ifs;
  ifs.open(fpath);
  if (!ifs) {
    return error::Internal("Failed to open file $0", fpath);
  }

  std::string line;
  bool ok = true;
  while (std::getline(ifs, line)) {
    std::vector<std::string_view> split = absl::StrSplit(line, " ", absl::SkipWhitespace());

    if (!split.empty() && split[0] == "cpu") {
      if (split.size() < kProcStatCPUNumFields) {
        return error::Unknown("Incorrect number of fields in proc/stat CPU");
      }

      ok &= absl::SimpleAtoi(split[KProcStatCPUKTimeField], &out->cpu_ktime_ns);
      ok &= absl::SimpleAtoi(split[KProcStatCPUUTimeField], &out->cpu_utime_ns);

      if (!ok) {
        return error::Unknown("Failed to parse proc/stat cpu info");
      }
      // We only need cpu. We can exit here.
      return Status::OK();
    }
  }

  // If we get here, we failed to extract system information.
  return error::NotFound("Could not extract system information");
}

Status ProcParser::ParseProcMemInfo(SystemStats* out) const {
  /**
   * Sample file:
   *   MemTotal:       65652452 kB
   *   MemFree:        19170960 kB
   *   MemAvailable:   52615288 kB
   * ...
   */
  CHECK(out != nullptr);
  std::string fpath = absl::Substitute("$0/meminfo", proc_base_path_);

  // Just to be safe when using offsetof, make sure object is standard layout.
  static_assert(std::is_standard_layout<SystemStats>::value);

  // clang-format off
  static absl::flat_hash_map<std::string_view, size_t> field_name_to_offset_map {
      {"MemTotal:", offsetof(SystemStats, mem_total_bytes)},
      {"MemFree:", offsetof(SystemStats, mem_free_bytes)},
      {"MemAvailable:", offsetof(SystemStats, mem_available_bytes)},
      {"Buffers:", offsetof(SystemStats, mem_buffer_bytes)},
      {"Cached:", offsetof(SystemStats, mem_cached_bytes)},
      {"SwapCached:", offsetof(SystemStats, mem_swap_cached_bytes)},
      {"Active:", offsetof(SystemStats, mem_active_bytes)},
      {"Inactive:", offsetof(SystemStats, mem_inactive_bytes)},
  };
  // clang-format on

  // This is a key value pair with a unit (that is always KB when present).
  constexpr int kKBToByteMultiplier = 1024;
  return ParseFromKeyValueFile(fpath, field_name_to_offset_map, reinterpret_cast<uint8_t*>(out),
                               kKBToByteMultiplier);
}

Status ProcParser::ParseFromKeyValueFile(
    const std::string& fpath,
    const absl::flat_hash_map<std::string_view, size_t>& field_name_to_value_map, uint8_t* out_base,
    int64_t field_value_multiplier) {
  std::ifstream ifs;
  ifs.open(fpath);
  if (!ifs) {
    return error::Internal("Failed to open file $0", fpath);
  }

  std::string line;
  size_t read_count = 0;
  while (std::getline(ifs, line)) {
    std::vector<std::string_view> split = absl::StrSplit(line, " ", absl::SkipWhitespace());
    // This is a key value pair with a unit (that is always KB when present).
    // If the number is 0 then the units are missing so we either have 2 or 3
    // for the width of the field.
    const int kMemInfoMinFields = 2;
    const int kMemInfoMaxFields = 3;

    if (split.size() >= kMemInfoMinFields && split.size() <= kMemInfoMaxFields) {
      const auto& key = split[0];
      const auto& val = split[1];

      const auto& it = field_name_to_value_map.find(key);
      // Key not found in map, we can just go to next iteration of loop.
      if (it == field_name_to_value_map.end()) {
        continue;
      }

      size_t offset = it->second;
      auto val_ptr = reinterpret_cast<int64_t*>(out_base + offset);
      bool ok = absl::SimpleAtoi(val, val_ptr);
      *val_ptr *= field_value_multiplier;

      if (!ok) {
        return error::Unknown("Failed to parse proc/meminfo");
      }

      // Check to see if we have read all the fields, if so we can skip the
      // rest. We assume no duplicates.
      if (read_count == field_name_to_value_map.size()) {
        break;
      }
    }
  }

  return Status::OK();
}

std::string ProcParser::GetPIDCmdline(int32_t pid) const {
  std::string fpath = absl::Substitute("$0/$1/cmdline", proc_base_path_, pid);
  std::ifstream ifs(fpath);
  if (!ifs) {
    return "";
  }

  std::string line = "";
  std::string cmdline = "";
  while (std::getline(ifs, line)) {
    cmdline += std::move(line);
  }

  // Strip out extra null character at the end of the string.
  if (!cmdline.empty() && cmdline[cmdline.size() - 1] == 0) {
    cmdline.pop_back();
  }

  // Replace all nulls with spaces. Sometimes the command line has
  // null to separate arguments and others it has spaces. We just make them all spaces
  // and leave it to upstream code to tokenize properly.
  std::replace(cmdline.begin(), cmdline.end(), static_cast<char>(0), ' ');

  return cmdline;
}

int64_t ProcParser::GetPIDStartTimeTicks(int32_t pid) const {
  const std::filesystem::path proc_pid_path =
      std::filesystem::path(proc_base_path_) / std::to_string(pid);
  return ::pl::system::GetPIDStartTimeTicks(proc_pid_path);
}

Status ProcParser::ReadProcPIDFDLink(int32_t pid, int32_t fd, std::string* out) const {
  std::string fpath = absl::Substitute("$0/$1/fd/$2", proc_base_path_, pid, fd);
  PL_ASSIGN_OR_RETURN(std::filesystem::path link, fs::ReadSymlink(fpath));
  *out = std::move(link);
  return Status::OK();
}

std::string_view LineWithPrefix(std::string_view content, std::string_view prefix) {
  const std::vector<std::string_view> lines = absl::StrSplit(content, "\n");
  for (const auto& line : lines) {
    if (absl::StartsWith(line, prefix)) {
      return line;
    }
  }
  return {};
}

// Looking for UIDs in <proc_path>/<pid>/status, the content looks like:
// $ cat /proc/2578/status
// Name:  apache2
// Umask: 0022
// State: S (sleeping)
// ...
// Uid: 33 33 33 33
// ...
Status ProcParser::ReadUIDs(int32_t pid, ProcUIDs* uids) const {
  std::filesystem::path proc_pid_status_path =
      std::filesystem::path(proc_base_path_) / std::to_string(pid) / "status";
  PL_ASSIGN_OR_RETURN(std::string content, pl::ReadFileToString(proc_pid_status_path));

  constexpr std::string_view kUIDPrefix = "Uid:";
  std::string_view uid_line = LineWithPrefix(content, kUIDPrefix);
  std::vector<std::string_view> fields =
      absl::StrSplit(uid_line, absl::ByAnyChar("\t "), absl::SkipEmpty());
  constexpr size_t kFieldCount = 5;
  if (fields.size() != kFieldCount) {
    return error::Internal("Proc path '$0' returns incorrect result '$1'",
                           proc_pid_status_path.string(), uid_line);
  }
  uids->real = fields[1];
  uids->effective = fields[2];
  uids->saved_set = fields[3];
  uids->filesystem = fields[4];
  return Status::OK();
}

// Looking for NSpid: in <proc_path>/<pid>/status, the content looks like:
// $ cat /proc/2578/status
// Name:  apache2
// Umask: 0022
// State: S (sleeping)
// ...
// NSpid: 33 33
// ...
//
// There may not be a second pid if the process is not running inside a namespace.
Status ProcParser::ReadNSPid(pid_t pid, std::vector<std::string>* ns_pids) const {
  std::filesystem::path proc_pid_status_path =
      std::filesystem::path(proc_base_path_) / std::to_string(pid) / "status";
  PL_ASSIGN_OR_RETURN(std::string content, pl::ReadFileToString(proc_pid_status_path));

  constexpr std::string_view kNSPidPrefix = "NStgid:";
  std::string_view ns_pid_line = LineWithPrefix(content, kNSPidPrefix);
  std::vector<std::string_view> fields =
      absl::StrSplit(ns_pid_line, absl::ByAnyChar("\t "), absl::SkipEmpty());
  if (fields.size() < 2) {
    return error::InvalidArgument("NSpid line in '$0' is invalid: '$1'",
                                  proc_pid_status_path.string(), ns_pid_line);
  }
  for (size_t i = 1; i < fields.size(); ++i) {
    ns_pids->push_back(std::string(fields[i]));
  }
  return Status::OK();
}

int64_t GetPIDStartTimeTicks(const std::filesystem::path& proc_pid_path) {
  const std::filesystem::path fpath = proc_pid_path / "stat";
  std::ifstream ifs;
  ifs.open(fpath.string());
  if (!ifs) {
    return 0;
  }

  std::string line;
  if (!std::getline(ifs, line)) {
    return 0;
  }

  std::vector<std::string_view> split = absl::StrSplit(line, " ", absl::SkipWhitespace());
  // We check less than in case more fields are added later.
  if (split.size() < kProcStatNumFields) {
    return 0;
  }

  int64_t start_time_ticks;
  if (!absl::SimpleAtoi(split[kProcStatStartTimeField], &start_time_ticks)) {
    return 0;
  }

  return start_time_ticks;
}

}  // namespace system
}  // namespace pl
