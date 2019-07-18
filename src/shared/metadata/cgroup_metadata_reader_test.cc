#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/common/testing/testing.h"
#include "src/shared/metadata/cgroup_metadata_reader.h"

namespace pl {
namespace md {

constexpr char kTestDataBasePath[] = "src/shared/metadata";

namespace {
std::string GetPathToTestDataFile(const std::string& fname) {
  return TestEnvironment::PathToTestDataFile(std::string(kTestDataBasePath) + "/" + fname);
}
}  // namespace

class CGroupMetadataReaderTest : public ::testing::Test {
 protected:
  virtual void SetUp() {
    std::string proc = GetPathToTestDataFile("testdata/proc1");
    std::string sysfs = GetPathToTestDataFile("testdata/sysfs1");

    md_reader_.reset(new CGroupMetadataReader(sysfs, proc, 100 /*ns_per_kernel_tick*/,
                                              128 /*clock_realtime_offset*/));
  }

  std::unique_ptr<CGroupMetadataReader> md_reader_;
};

TEST_F(CGroupMetadataReaderTest, read_pid_list) {
  std::vector<uint32_t> pid_list;
  ASSERT_OK(md_reader_->ReadPIDList(PodQOSClass::kBestEffort, "abcd", "c123", &pid_list));
  EXPECT_THAT(pid_list, ::testing::ElementsAre(123, 456, 789));
}

TEST_F(CGroupMetadataReaderTest, read_pid_metadata) {
  PIDMetadata md;
  ASSERT_OK(md_reader_->ReadPIDMetadata(32391, &md));
  EXPECT_EQ(32391, md.pid);
  // This is the time from the file * 100 + 128.
  EXPECT_EQ(8001981028, md.start_time_ns);
  EXPECT_THAT(md.cmdline_args,
              "/usr/lib/slack/slack --force-device-scale-factor=1.5 --high-dpi-support=1");
}

TEST_F(CGroupMetadataReaderTest, read_pid_metadata_null) {
  PIDMetadata md;
  ASSERT_OK(md_reader_->ReadPIDMetadata(79690, &md));
  EXPECT_THAT(md.cmdline_args, "/usr/lib/at-spi2-core/at-spi2-registryd --use-gnome-session");
}

TEST_F(CGroupMetadataReaderTest, cgroup_proc_file_path) {
  EXPECT_EQ(
      "/pl/sys/cgroup/cpu,cpuacct/kubepods/burstable/podabcd/c123/cgroup.procs",
      CGroupMetadataReader::CGroupProcFilePath("/pl/sys", PodQOSClass::kBurstable, "abcd", "c123"));
  EXPECT_EQ("/pl/sys/cgroup/cpu,cpuacct/kubepods/besteffort/podabcd/c123/cgroup.procs",
            CGroupMetadataReader::CGroupProcFilePath("/pl/sys", PodQOSClass::kBestEffort, "abcd",
                                                     "c123"));
  EXPECT_EQ("/pl/sys/cgroup/cpu,cpuacct/kubepods/podabcd/c123/cgroup.procs",
            CGroupMetadataReader::CGroupProcFilePath("/pl/sys", PodQOSClass::kGuaranteed, "abcd",
                                                     "c123"));
}

}  // namespace md
}  // namespace pl
