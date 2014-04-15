#include <future>
#include <thread>
#include <atomic>
#include <tuple>
#include <algorithm>

#include "DiskCleanupTest.h"
#include "DiskCleanup.h"
#include "DiskPacketCapture.h"
#include "tempFileCreate.h"
#include "SendStats.h"
#include "MockConf.h"
#include "MockConfMaster.h"
#include "MockElasticSearch.h"
#include "MockSendStats.h"
#include "MockStopWatch.h"

#ifdef LR_DEBUG
static MockConfMaster mConfMaster;
static MockConfSlave mConf;

TEST_F(DiskCleanupTest, SetupMockConfSlave) {
   ProcessManager::InstanceWithConfMaster(mConfMaster);
}
#else
static ConfSlave& mConf(ConfSlave::Instance());
#endif
#ifdef LR_DEBUG
TEST_F(DiskCleanupTest, MarkFileAsRemovedInES) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   MockElasticSearch es(transport, false);
   IdsAndIndexes recordsToUpdate;
   networkMonitor::DpiMsgLR updateMsg;
   EXPECT_FALSE(cleanup.MarkFilesAsRemovedInES(recordsToUpdate, updateMsg, es));
   recordsToUpdate.emplace_back("123456789012345678901234567890123456", "foo");
   es.mFakeBulkUpdate = true;
   es.mBulkUpdateResult = false;
   EXPECT_FALSE(cleanup.MarkFilesAsRemovedInES(recordsToUpdate, updateMsg, es));
   es.mBulkUpdateResult = true;
   EXPECT_TRUE(cleanup.MarkFilesAsRemovedInES(recordsToUpdate, updateMsg, es));
}
// Could also be DISABLED since it used two partitions for verifying that the actual
// partition calculation is OK. On our test system (Jenkins?) all is on the
// same partition so in this case we do not run this test
//
// There is no point in mocking this and using folders on the same partition
// since the test is for verifying how it works on partitions

TEST_F(DiskCleanupTest, SystemTest_GetPcapStoreUsageManyLocations) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   DiskUsage home("/home/tmp/TooMuchPcap");
   DiskUsage root("/tmp/TooMuchPcap");
   if (home.FileSystemID() != root.FileSystemID()) {
      mConf.mConfLocation += ""; // ensuring that the Conf returned is the MockConf
      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);
      cleanup.mUseMockConf = true;
      cleanup.mRealFilesSystemAccess = true;
      cleanup.mMockedConf.mOverrideGetPcapCaptureLocations = true;

      tempFileCreate scopedHome(cleanup.mMockedConf, "/home/tmp/TooMuchPcap");
      tempFileCreate scopedRoot(cleanup.mMockedConf, "/tmp/TooMuchPcap");

      ASSERT_TRUE(scopedHome.Init());
      ASSERT_TRUE(scopedRoot.Init());

      // explicit writing mConf.mConf to make it clear what MockConf we are using
      cleanup.mMockedConf.mPCapCaptureLocations.clear();
      cleanup.mMockedConf.mPCapCaptureLocations.push_back(scopedHome.mTestDir.str());
      cleanup.mMockedConf.mPCapCaptureLocations.push_back(scopedRoot.mTestDir.str());


      ASSERT_EQ(cleanup.mMockedConf.GetPcapCaptureLocations().size(), 2);
      ASSERT_EQ(cleanup.mMockedConf.GetPcapCaptureLocations()[0], scopedHome.mTestDir.str());
      ASSERT_EQ(cleanup.mMockedConf.GetPcapCaptureLocations()[1], scopedRoot.mTestDir.str());
      EXPECT_EQ(cleanup.mMockedConf.GetFirstPcapCaptureLocation(), scopedHome.mTestDir.str());

      // This is what happens in DiskClean for Pcap calculations
      ASSERT_EQ(cleanup.GetConf().GetPcapCaptureLocations().size(), 2);
      ASSERT_EQ(cleanup.GetConf().GetFirstPcapCaptureLocation(), scopedHome.mTestDir.str());
      ASSERT_EQ(cleanup.GetConf().GetPcapCaptureLocations()[0], scopedHome.mTestDir.str());
      ASSERT_EQ(cleanup.GetConf().GetPcapCaptureLocations()[1], scopedRoot.mTestDir.str());
      ASSERT_EQ(cleanup.GetConf().GetProbeLocation(), "/usr/local/probe/");

      // Using 2 different partitions for pcaps
      auto size = MemorySize::MB;
      cleanup.GetPcapStoreUsage(stats, size); // high granularity in case something changes on the system
      DiskUsage atRoot{scopedRoot.mTestDir.str()};
      DiskUsage atHome{scopedHome.mTestDir.str()};

      auto isFree = atRoot.DiskFree(size) + atHome.DiskFree(size);
      EXPECT_NEAR(stats.pcapDiskInGB.Free, isFree, 50) << ". home: " << atHome.DiskFree(size) << ". root:" << atRoot.DiskFree(size);


      auto isUsed1 = atRoot.RecursiveFolderDiskUsed(scopedRoot.mTestDir.str(), size);
      auto isUsed2 = atHome.RecursiveFolderDiskUsed(scopedHome.mTestDir.str(), size);
      auto isUsed = isUsed1 + isUsed2;
      EXPECT_EQ(stats.pcapDiskInGB.Used, isUsed) << ". home: " << isUsed2 << ". root:" << isUsed1;

      auto isTotal = atRoot.DiskTotal(size) + atHome.DiskTotal(size);
      EXPECT_EQ(stats.pcapDiskInGB.Total, isTotal) << ". home: " << atHome.DiskTotal(size) << ". root:" << atRoot.DiskTotal(size);


      // Sanity check. Add a 10MFile and see that increments happen
      std::string make1MFileFile = "dd bs=1024 count=10240 if=/dev/zero of=";
      make1MFileFile += scopedHome.mTestDir.str();
      make1MFileFile += "/10MFile";
      EXPECT_EQ(0, system(make1MFileFile.c_str()));
      DiskUsage atHome2(scopedHome.mTestDir.str());
      size_t usedMByte = atHome2.RecursiveFolderDiskUsed(scopedHome.mTestDir.str(), size);
      cleanup.GetPcapStoreUsage(stats, size);
      EXPECT_EQ(stats.pcapDiskInGB.Used, isUsed + 10);
      EXPECT_EQ(stats.pcapDiskInGB.Used, usedMByte + atRoot.RecursiveFolderDiskUsed(scopedRoot.mTestDir.str(), size));
   }
}
TEST_F(DiskCleanupTest, LastIterationAmount) {

   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);

   EXPECT_TRUE(cleanup.LastIterationAmount(0));
   EXPECT_TRUE(cleanup.LastIterationAmount(1));
   EXPECT_TRUE(cleanup.LastIterationAmount(1000));
   EXPECT_FALSE(cleanup.LastIterationAmount(1001));

   mConf.mConfLocation = "resources/test.yaml.DiskCleanup2"; // file limit 1
   cleanup.ResetConf();
   EXPECT_TRUE(cleanup.LastIterationAmount(0));
   EXPECT_TRUE(cleanup.LastIterationAmount(1));
   EXPECT_FALSE(cleanup.LastIterationAmount(2));
   EXPECT_FALSE(cleanup.LastIterationAmount(1001));

   mConf.mConfLocation = "resources/test.yaml.DiskCleanup0"; // file limit 0
   cleanup.ResetConf();
   EXPECT_TRUE(cleanup.LastIterationAmount(0));
   EXPECT_TRUE(cleanup.LastIterationAmount(1));
   EXPECT_TRUE(cleanup.LastIterationAmount(2));
   EXPECT_TRUE(cleanup.LastIterationAmount(1001));
}

TEST_F(DiskCleanupTest, GetProbeDiskUsage) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   cleanup.mRealFilesSystemAccess = true;
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup9";
   cleanup.ResetConf();
   Conf& conf = cleanup.GetConf();


   EXPECT_EQ(conf.GetPcapCaptureLocations()[0], "/tmp/TooMuchPcap/");
   EXPECT_EQ(conf.GetProbeLocation(), "/tmp/TooMuchPcap");

   // Same partition: disk usage for pcap is calulcated on the folder itself
   // free and total should be huge since it is on the root partition
   DiskCleanup::DiskSpace pcapDiskInKByte{0, 0, 0};
   DiskCleanup::DiskSpace probeDiskInKByte{0, 0, 0};

   cleanup.GetPcapStoreUsage(stats, MemorySize::KByte);
   cleanup.GetProbeFileSystemInfo(stats, MemorySize::KByte);

   // Same partition. For "free" and "total" used memory should be the same for 
   //  GetTotal..., GetProbe... and getPcap
   //  However... in a Jenkins/test environment we see frequently that possible temporary
   //  files are created that make this test fail. For this reason we allow a 1% diff.
   auto maxFree = std::max(stats.pcapDiskInGB.Free, stats.probeDiskInGB.Free);
   auto maxFreeDiffAllowed = maxFree/100;
   EXPECT_NE(maxFreeDiffAllowed, 0);
   EXPECT_NEAR(stats.pcapDiskInGB.Free, stats.probeDiskInGB.Free, maxFreeDiffAllowed);

   EXPECT_EQ(stats.pcapDiskInGB.Total, stats.probeDiskInGB.Total);
   EXPECT_NE(stats.pcapDiskInGB.Used, stats.probeDiskInGB.Used); // pcap is the folder, probe is the partition
   EXPECT_EQ(stats.pcapDiskInGB.Used, 4); // folder takes up space
   EXPECT_NE(stats.probeDiskInGB.Used, 4); // probe is the whole partition
   EXPECT_TRUE(stats.probeDiskInGB.Used > 4);

}
#endif

TEST_F(DiskCleanupTest, TooMuchPCap) {

#ifdef LR_DEBUG
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup1";

   if (geteuid() == 0) {

      MockElasticSearch es(false);

      es.mUpdateDocAlwaysPasses = true;
      es.RunQueryGetIdsAlwaysPasses = true;
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup1";

      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);
      cleanup.ResetConf();
      cleanup.mRealFilesSystemAccess = true;
      cleanup.RecalculatePCapDiskUsed(stats, es);
      ASSERT_FALSE(cleanup.TooMuchPCap(stats));
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup2";
      cleanup.ResetConf();
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup3";

      cleanup.ResetConf();

      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup4";
      cleanup.ResetConf();
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));

      DiskUsage usage(testDir.str());

      // Empty folder:
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), 4096);
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte), 4); // empty folder eq 4 KByte
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB), 0); // empty folder eq 4 Bytes
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::GB), 0); // empty folder eq 4 Bytes

      std::string makeSmallFile = "touch ";
      makeSmallFile += testDir.str();
      makeSmallFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"; // 1 empty file
      EXPECT_EQ(0, system(makeSmallFile.c_str()));
      time_t timeFirst = std::time(NULL);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aPcapUsageInMB, 0); // 0MB
      EXPECT_EQ(stats.aTotalFiles, 1);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats)); // 1 file, limit 1 file

      // empty file, no different in folder size
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), 4096);
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte), 4);
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB), 0);
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::GB), 0);


      mConf.mConfLocation = "resources/test.yaml.DiskCleanup5";
      cleanup.ResetConf();
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      // complete the 1MB
      std::string make1MFileFile = "dd bs=1024 count=1024 if=/dev/zero of=";
      make1MFileFile += testDir.str();
      make1MFileFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1M";
      EXPECT_EQ(0, system(make1MFileFile.c_str()));
      time_t timeSecond = std::time(NULL);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), 1052672);
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte), 1024 + 4);
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB), 1); //  // 2 files: 1MB + (4KByte folder overhead)
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::GB), 0);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup6";
      cleanup.ResetConf();
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));


      // 1 byte less than 1 MB. Keep subtracting that from the calculations below
      size_t byte = 1;
      make1MFileFile = "dd bs=1048575 count=1 if=/dev/zero of=";
      make1MFileFile += testDir.str();
      make1MFileFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1";
      EXPECT_EQ(0, system(make1MFileFile.c_str()));
      time_t timeThird = std::time(NULL);
      std::this_thread::sleep_for(std::chrono::seconds(1));

      // 3 files: 2MB + (4KByte folder overhead) - 1byte
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte),
              (2 << B_TO_MB_SHIFT) + (4 << B_TO_KB_SHIFT) - byte);

      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup7";
      cleanup.ResetConf();

      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      size_t spaceSaved(0);
      auto location = cleanup.GetConf().GetFirstPcapCaptureLocation();
      EXPECT_EQ(testDir.str() + "/", location);
      std::this_thread::sleep_for(std::chrono::seconds(1));

      EXPECT_EQ(1, cleanup.BruteForceCleanupOfOldFiles(location, timeSecond, spaceSaved)); // 2 files, empty file removed
      EXPECT_EQ(0, spaceSaved);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      EXPECT_EQ(2, stats.aTotalFiles);

      // 2MB + (4KByte folder overhead) - 1byte
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte),
              (2 << B_TO_MB_SHIFT) + (4 << B_TO_KB_SHIFT) - byte);

      mConf.mConfLocation = "resources/test.yaml.DiskCleanup8";
      cleanup.ResetConf();
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      EXPECT_EQ(stats.aTotalFiles, 2);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      EXPECT_EQ(1, cleanup.BruteForceCleanupOfOldFiles(testDir.str(), timeThird, spaceSaved));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(1, stats.aTotalFiles);
      EXPECT_EQ(1, spaceSaved); //left is 1 file: 1MB-1byte + (4KByte folder overhead) 
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte),
              (1 << B_TO_MB_SHIFT) + (4 << B_TO_KB_SHIFT) - byte);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats)); // 1MB limit vs 1MB-1byte + 4KByte

      mConf.mConfLocation = "resources/test.yaml.DiskCleanup9"; // 1MB limit, 1 file limit
      cleanup.ResetConf();
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      std::this_thread::sleep_for(std::chrono::seconds(1));
      EXPECT_EQ(1, cleanup.BruteForceCleanupOfOldFiles(testDir.str(), std::time(NULL), spaceSaved));
      EXPECT_EQ(1, spaceSaved); // 
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte),
              (0 << B_TO_MB_SHIFT) + (4 << B_TO_KB_SHIFT));

      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(0, stats.aTotalFiles);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));

   }
#endif
}

TEST_F(DiskCleanupTest, TooMuchPCapPrecursor) {
#ifdef LR_DEBUG

   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   DiskUsage usage(testDir.str());
   // Empty folder:
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), 4096); // empty folder eq 4 KByte
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte), 4); // empty folder eq 4 KByte
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB), 0); // empty folder eq 4 Bytes
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::GB), 0); // empty folder eq 4 Bytes

   std::string almost1MFileFile = "dd bs=1024 count=1000 if=/dev/zero of=";
   almost1MFileFile += testDir.str();
   almost1MFileFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbAlmost1M";
   EXPECT_EQ(0, system(almost1MFileFile.c_str()));
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), (1024 * 1000) + 4096); // written explicitly 
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte), 1004);
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB), 0); //  // 2 files: 1MB + (4KByte folder overhead)
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::GB), 0);

   // 1MB
   std::string upTo1MFile = "dd bs=1024 count=20 if=/dev/zero of=";
   upTo1MFile += testDir.str();
   upTo1MFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbUpTo1M";
   EXPECT_EQ(0, system(upTo1MFile.c_str()));
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), 1024 * 1024);
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte), 1024);
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB), 1); //  // 2 files: 1MB + (4KByte folder overhead)
   EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::GB), 0);

#endif
}

#ifdef LR_DEBUG

TEST_F(DiskCleanupTest, RemoveDuplicateUpdatesFromLastUpdate) {
   PathAndFileNames esFilesToRemove;
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);


   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test1", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test2", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test3", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test1", "aaa"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test1", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   ASSERT_EQ(5, esFilesToRemove.size());
   cleanup.RemoveDuplicateUpdatesFromLastUpdate(esFilesToRemove);
   ASSERT_EQ(5, esFilesToRemove.size());
   cleanup.RemoveDuplicateUpdatesFromLastUpdate(esFilesToRemove);
   ASSERT_EQ(0, esFilesToRemove.size());
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test1", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test2", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test3", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test1", "aaa"));
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test1", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   cleanup.RemoveDuplicateUpdatesFromLastUpdate(esFilesToRemove);
   ASSERT_EQ(5, esFilesToRemove.size());
   esFilesToRemove.insert(std::make_tuple<std::string, std::string>("test5", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   cleanup.RemoveDuplicateUpdatesFromLastUpdate(esFilesToRemove);
   ASSERT_EQ(1, esFilesToRemove.size());
   EXPECT_EQ("test5", std::get<0>(*esFilesToRemove.begin()));
}

TEST_F(DiskCleanupTest, RecalculatePCapDiskUsed) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);

   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();
   cleanup.RecalculatePCapDiskUsed(stats, es);
   EXPECT_EQ(1234, stats.aTotalFiles);
   cleanup.DelegateGetFileCountFromES(0, false);
   cleanup.RecalculatePCapDiskUsed(stats, es);
   EXPECT_EQ(1234, stats.aTotalFiles);
   cleanup.DelegateGetFileCountFromES(0, true);
   cleanup.RecalculatePCapDiskUsed(stats, es);
   EXPECT_EQ(0, stats.aTotalFiles);

   cleanup.GetFileCountFromESThrows();
   cleanup.RecalculatePCapDiskUsed(stats, es);
   EXPECT_EQ(0, stats.aTotalFiles);
   EXPECT_EQ(0, stats.aPcapUsageInMB);
}

// Added since it after a merge mixed up the Free, Total and Used values

TEST_F(DiskCleanupTest, RecalculatePCapDiskUsedMockUsage) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);


   // Just some random values that are independent of each other
   //    (and honestly should not even make sense)
   // free: 2048 (2GB), total: 5000000 (4882 GB), used: 5120 (5GB)
   DiskCleanup::DiskSpace disk;
   disk.Free = 2048;
   disk.Total = 5000000;
   disk.Used = 5120;
   DiskCleanup::StatInfo mockStats;
   mockStats.pcapDiskInGB = disk;
   EXPECT_CALL(cleanup, GetPcapStoreUsage(_, _))
           .WillOnce(SetArgReferee<0>(mockStats));
   cleanup.DelegateGetFileCountFromES(0, true);
   DiskCleanup::StatInfo readStats;
   cleanup.RecalculatePCapDiskUsed(readStats, es);
   EXPECT_EQ(readStats.pcapDiskInGB.Free, 2);
   EXPECT_EQ(readStats.pcapDiskInGB.Total, 4882);
   EXPECT_EQ(readStats.pcapDiskInGB.Used, 5);
}

TEST_F(DiskCleanupTest, RemoveOldestPCapFilesInESNoFiles) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);
   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();

   es.DelegateGetOldestNFiles(bogusFileList, bogusIdsAndIndex, bogusTime);

   size_t maxToRemove(0);
   size_t spaceSavedInMB(123);
   time_t theOldestTime(0);
   size_t filesPerIteration(10);

   EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(maxToRemove, filesPerIteration, es, spaceSavedInMB, theOldestTime, stats));
   EXPECT_EQ(0, spaceSavedInMB);
   EXPECT_NE(0L, theOldestTime);
}

TEST_F(DiskCleanupTest, RemoveOldestPCapFilesInESNoRemove) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);
   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();
   es.DelegateGetOldestNFiles(bogusFileList, bogusIdsAndIndex, bogusTime);

   size_t maxToRemove(0);
   size_t spaceSavedInMB(123);
   time_t theOldestTime(0);
   size_t filesPerIteration(10);

   cleanup.DelegateRemoveFiles(10);
   maxToRemove = 10;

   cleanup.DelegateMarkFilesAsRemovedInESExpectNTimes(1, true);
   EXPECT_EQ(10, cleanup.RemoveOldestPCapFilesInES(maxToRemove, filesPerIteration, es, spaceSavedInMB, theOldestTime, stats));
   EXPECT_EQ(bogusTime, theOldestTime);
}

TEST_F(DiskCleanupTest, RemoveOldestPCapFilesInESMarkMatchMax) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);
   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();
   es.DelegateGetOldestNFiles(bogusFileList, bogusIdsAndIndex, bogusTime);

   size_t maxToRemove(0);
   size_t spaceSavedInMB(123);
   time_t theOldestTime(0);
   size_t filesPerIteration(10);

   maxToRemove = 10;
   cleanup.DelegateRemoveFiles(0);
   cleanup.DelegateMarkFilesAsRemovedInESExpectNTimes(1, true);
   EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(maxToRemove, filesPerIteration, es, spaceSavedInMB, theOldestTime, stats));
   EXPECT_EQ(10, cleanup.mRecordsMarked.size());
}

TEST_F(DiskCleanupTest, RemoveOldestPCapFilesInESShortIterations) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);
   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();
   bogusIdsAndIndex.clear();
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "1"));
   es.DelegateGetOldestNFiles(bogusFileList, bogusIdsAndIndex, bogusTime);

   size_t maxToRemove(0);
   size_t spaceSavedInMB(123);
   time_t theOldestTime(0);
   size_t filesPerIteration(1);

   maxToRemove = 10;
   cleanup.DelegateRemoveFiles(0);
   cleanup.DelegateMarkFilesAsRemovedInESExpectNTimes(10, true);
   EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(maxToRemove, filesPerIteration, es, spaceSavedInMB, theOldestTime, stats));
   EXPECT_EQ(1, cleanup.mRecordsMarked.size());
}

TEST_F(DiskCleanupTest, RemoveOldestPCapFilesInESDoubleReturns) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);
   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();
   bogusIdsAndIndex.clear();
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "1"));
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "2"));
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "3"));
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "4"));
   es.DelegateGetOldestNFiles(bogusFileList, bogusIdsAndIndex, bogusTime);

   size_t maxToRemove(0);
   size_t spaceSavedInMB(123);
   time_t theOldestTime(0);
   size_t filesPerIteration(2);

   maxToRemove = 10;
   cleanup.DelegateRemoveFiles(0);
   cleanup.DelegateMarkFilesAsRemovedInESExpectNTimes(10, true);
   EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(maxToRemove, filesPerIteration, es, spaceSavedInMB, theOldestTime, stats));
   EXPECT_EQ(2, cleanup.mRecordsMarked.size());
}

TEST_F(DiskCleanupTest, RemoveOldestPCapFilesInESOddIterations) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   GMockDiskCleanup cleanup(mConf, processClient);
   MockBoomStick transport("ipc://tmp/foo.ipc");
   GMockElasticSearch es(transport, false);
   cleanup.DelegateGetFileCountFromES(1234, true);
   cleanup.DelegateGetPcapStoreUsage();
   bogusIdsAndIndex.clear();
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "1"));
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "2"));
   bogusIdsAndIndex.push_back(std::make_pair<std::string, std::string>("abc", "3"));
   es.DelegateGetOldestNFiles(bogusFileList, bogusIdsAndIndex, bogusTime);

   size_t maxToRemove(0);
   size_t spaceSavedInMB(123);
   time_t theOldestTime(0);
   size_t filesPerIteration(3);

   maxToRemove = 10;
   cleanup.DelegateRemoveFiles(0);
   cleanup.DelegateMarkFilesAsRemovedInESExpectNTimes(4, true);
   EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(maxToRemove, filesPerIteration, es, spaceSavedInMB, theOldestTime, stats));
   EXPECT_EQ(3, cleanup.mRecordsMarked.size());
}

TEST_F(DiskCleanupTest, RemoveFile) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);

   std::string path;
   path += testDir.str();
   path += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"; // 1 empty file

   mConf.mConf.mPCapCaptureLocations.push_back(testDir.str());
   std::string makeADir;
   ASSERT_EQ(0, system(makeADir.c_str()));
   std::string makeSmallFile = "touch ";
   makeSmallFile += path;

   EXPECT_EQ(0, system(makeSmallFile.c_str()));
   struct stat filestat;
   EXPECT_TRUE(stat(path.c_str(), &filestat) == 0);
   EXPECT_TRUE(cleanup.RemoveFile(path));
   EXPECT_FALSE(stat(path.c_str(), &filestat) == 0);
   EXPECT_FALSE(cleanup.RemoveFile(path)); // Missing file gives false return
   EXPECT_FALSE(stat(path.c_str(), &filestat) == 0);
}

TEST_F(DiskCleanupTest, RemoveFiles) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   PathAndFileNames filesToRemove;
   size_t spaceSavedInMB(99999);
   struct stat filestat;
   std::string path;
   path += testDir.str();
   path += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"; // 1 empty file

   mConf.mConf.mPCapCaptureLocations.push_back(testDir.str());
   std::string makeADir;
   ASSERT_EQ(0, system(makeADir.c_str()));
   std::string makeSmallFile = "touch ";
   makeSmallFile += path;

   EXPECT_EQ(0, system(makeSmallFile.c_str()));
   size_t filesNotFound;
   EXPECT_EQ(0, cleanup.RemoveFiles(filesToRemove, spaceSavedInMB, filesNotFound));
   EXPECT_EQ(0, spaceSavedInMB);
   spaceSavedInMB = 999999;
   filesToRemove.insert(std::make_tuple<std::string, std::string>(path.c_str(), "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
   EXPECT_TRUE(stat(path.c_str(), &filestat) == 0);
   EXPECT_EQ(0, cleanup.RemoveFiles(filesToRemove, spaceSavedInMB, filesNotFound));
   EXPECT_EQ(0, spaceSavedInMB);
   EXPECT_FALSE(stat(path.c_str(), &filestat) == 0);
   spaceSavedInMB = 999999;
   cleanup.mFakeRemove = true;
   cleanup.mRemoveResult = false;
   EXPECT_EQ(0, system(makeSmallFile.c_str()));
   EXPECT_EQ(1, cleanup.RemoveFiles(filesToRemove, spaceSavedInMB, filesNotFound));
   EXPECT_EQ(0, spaceSavedInMB);

   cleanup.mFakeRemove = false;

   std::string make1MFileFile = "dd bs=1024 count=1024 if=/dev/zero of=";
   path = testDir.str();
   path += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1M";
   make1MFileFile += path;

   EXPECT_EQ(0, system(make1MFileFile.c_str()));

   filesToRemove.clear();
   filesToRemove.insert(std::make_tuple<std::string, std::string>(path.c_str(), "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1M"));
   EXPECT_EQ(0, cleanup.RemoveFiles(filesToRemove, spaceSavedInMB, filesNotFound));
   EXPECT_EQ(1, spaceSavedInMB);

   EXPECT_FALSE(stat(path.c_str(), &filestat) == 0);

   EXPECT_EQ(0, system(make1MFileFile.c_str()));
   cleanup.mFakeIsShutdown = true;
   cleanup.mIsShutdownResult = true;
   EXPECT_EQ(0, cleanup.RemoveFiles(filesToRemove, spaceSavedInMB, filesNotFound));
   EXPECT_EQ(0, spaceSavedInMB);
   EXPECT_TRUE(stat(path.c_str(), &filestat) == 0);
}

TEST_F(DiskCleanupTest, RemoveOlderFilesFromPath) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   PathAndFileNames filesToFind;
   std::string path;
   path += testDir.str();
   path += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
   std::string makeSmallFile = "touch ";
   makeSmallFile += path;
   size_t additionalRemoved;
   EXPECT_EQ(0, cleanup.RemoveOlderFilesFromPath(testDir.str(), std::time(NULL), additionalRemoved));

   EXPECT_EQ(0, system(makeSmallFile.c_str()));
   std::this_thread::sleep_for(std::chrono::seconds(1)); // increase timestamp
   cleanup.mFakeIsShutdown = true;
   cleanup.mIsShutdownResult = true;
   EXPECT_EQ(0, cleanup.RemoveOlderFilesFromPath(testDir.str(), std::time(NULL), additionalRemoved));

   cleanup.mFakeIsShutdown = false;
   EXPECT_EQ(0, cleanup.RemoveOlderFilesFromPath("/thisPathIsGarbage", 0, additionalRemoved));
   std::this_thread::sleep_for(std::chrono::seconds(1));
   EXPECT_EQ(1, cleanup.RemoveOlderFilesFromPath(testDir.str(), std::time(NULL), additionalRemoved));
}

TEST_F(DiskCleanupTest, TimeToForceAClean) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   MockStopWatch lastClean;
   cleanup.SetLastForcedClean(lastClean);
   EXPECT_FALSE(cleanup.TimeForBruteForceCleanup(lastClean));
   lastClean.SetTimeForwardSeconds((20*60) + 1);
   EXPECT_TRUE(cleanup.TimeForBruteForceCleanup(lastClean));
}

TEST_F(DiskCleanupTest, WayTooManyFiles) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);


   mConf.mConfLocation = "resources/test.yaml.DiskCleanup1";
   cleanup.ResetConf();
   stats.aTotalFiles = 0;
   EXPECT_FALSE(cleanup.WayTooManyFiles(stats));
   stats.aTotalFiles = 59999;
   EXPECT_FALSE(cleanup.WayTooManyFiles(stats));
   stats.aTotalFiles = 60000;
   EXPECT_FALSE(cleanup.WayTooManyFiles(stats));

   mConf.mConfLocation = "resources/test.yaml.DiskCleanup0"; // file limit 0
   cleanup.ResetConf();
   stats.aTotalFiles = 0;
   EXPECT_FALSE(cleanup.WayTooManyFiles(stats)); // Don't add more to 100% of the files when we have a target of 0
}

TEST_F(DiskCleanupTest, CalculateNewTotalFiles) {

   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   EXPECT_EQ(0, cleanup.CalculateNewTotalFiles(0, 0, 0));
   EXPECT_EQ(0, cleanup.CalculateNewTotalFiles(0, 0, 1));
   EXPECT_EQ(0, cleanup.CalculateNewTotalFiles(0, 1, 0));
   EXPECT_EQ(0, cleanup.CalculateNewTotalFiles(0, 1, 1));
   EXPECT_EQ(1, cleanup.CalculateNewTotalFiles(1, 0, 0));
   EXPECT_EQ(1, cleanup.CalculateNewTotalFiles(1, 0, 1));
   EXPECT_EQ(0, cleanup.CalculateNewTotalFiles(1, 1, 0));
   EXPECT_EQ(1, cleanup.CalculateNewTotalFiles(1, 1, 1));
}

TEST_F(DiskCleanupTest, IterationTargetToRemove) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup11"; // file limit 30000000
   cleanup.ResetConf();
   stats.aTotalFiles = 0;
   EXPECT_EQ(0, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 1;
   EXPECT_EQ(1000, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 49999;
   EXPECT_EQ(1000, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 50001;
   EXPECT_EQ(1001, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 5000100;
   EXPECT_EQ(100002, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 1000000000;
   EXPECT_EQ(500000, cleanup.IterationTargetToRemove(stats));

   mConf.mConfLocation = "resources/test.yaml.DiskCleanup0"; // file limit 0
   cleanup.ResetConf();
   stats.aTotalFiles = 0;
   EXPECT_EQ(0, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 1;
   EXPECT_EQ(1, cleanup.IterationTargetToRemove(stats));
   stats.aTotalFiles = 50100;
   EXPECT_EQ(50100, cleanup.IterationTargetToRemove(stats));

}

TEST_F(DiskCleanupTest, CleanupMassiveOvershoot) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup0"; // file limit 0
   cleanup.ResetConf();
   stats.aTotalFiles = 1000;
   EXPECT_EQ(100, cleanup.CleanupMassiveOvershoot(0, stats));
   EXPECT_EQ(110, cleanup.CleanupMassiveOvershoot(10, stats));
   EXPECT_EQ(1000, cleanup.CleanupMassiveOvershoot(901, stats));
   EXPECT_EQ(1000, cleanup.CleanupMassiveOvershoot(10000, stats));
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup1"; // file limit 30000
   cleanup.ResetConf();
   stats.aTotalFiles = 30000 + 1000;
   EXPECT_EQ(100, cleanup.CleanupMassiveOvershoot(0, stats));
   EXPECT_EQ(110, cleanup.CleanupMassiveOvershoot(10, stats));
   EXPECT_EQ(1000, cleanup.CleanupMassiveOvershoot(901, stats));
   EXPECT_EQ(1000, cleanup.CleanupMassiveOvershoot(10000, stats));
}

TEST_F(DiskCleanupTest, DISABLED_ValgrindGetOrderedMapOfFiles) {
   MockElasticSearch es(false);
   es.mUpdateDocAlwaysPasses = true;

   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   size_t maxToRemove = 5000;
   size_t filesRemoved(0);
   size_t spaceRemoved(0);
   boost::filesystem::path path = "/build/A/directory/with/files/as/part/of/this/test";
   for (int i = 0; i < 1 && !zctx_interrupted; i++) {

      cleanup.RemoveOlderFilesFromPath(path, std::time(NULL), spaceRemoved);

      std::cout << "iteration " << i << std::endl;
   }

}
#endif

TEST_F(DiskCleanupTest, TooMuchPCapsAtManyLocations) {
#ifdef LR_DEBUG
   if (geteuid() == 0) {
      MockElasticSearch es(false);
      es.mUpdateDocAlwaysPasses = true;
      es.RunQueryGetIdsAlwaysPasses = true;

      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);
      cleanup.mRealFilesSystemAccess = true;
      mConf.mConfLocation = "resources/test.yaml.GetPcapStoreUsage1";
      ASSERT_EQ(0, system("mkdir -p /tmp/TooMuchPcap/pcap0"));
      ASSERT_EQ(0, system("mkdir -p /tmp/TooMuchPcap/pcap1"));
      cleanup.ResetConf();
      Conf& conf = cleanup.GetConf();
      auto locations = conf.GetPcapCaptureLocations();
      ASSERT_EQ(locations.size(), 2) << conf.GetFirstPcapCaptureLocation();
      ASSERT_EQ(locations[0], "/tmp/TooMuchPcap/pcap0/");
      ASSERT_EQ(locations[1], "/tmp/TooMuchPcap/pcap1/");

      cleanup.RecalculatePCapDiskUsed(stats, es);
      ASSERT_FALSE(cleanup.TooMuchPCap(stats));
      EXPECT_EQ(stats.aTotalFiles, 0);

      std::string makeFile1 = "dd bs=1024 count=1024 if=/dev/zero of=";
      makeFile1 += locations[0];
      makeFile1 += "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1";
      std::string makeFile2 = "dd bs=1024 count=1024 if=/dev/zero of=";
      makeFile2 += locations[1];
      makeFile2 += "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1";
      EXPECT_EQ(system(makeFile1.c_str()), 0) << makeFile1;
      EXPECT_EQ(system(makeFile2.c_str()), 0) << makeFile2;
      std::this_thread::sleep_for(std::chrono::seconds(1));
      time_t timeFirst = std::time(NULL);


      EXPECT_TRUE(boost::filesystem::exists(locations[0] + "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1"));
      EXPECT_TRUE(boost::filesystem::exists(locations[1] + "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1"));

      // Clean it up
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aTotalFiles, 2);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      size_t spaceSaved = 0;

      // Remove the first file
      EXPECT_EQ(1, cleanup.BruteForceCleanupOfOldFiles(locations[0], timeFirst, spaceSaved));
      EXPECT_EQ(1, spaceSaved);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(1, stats.aTotalFiles);
      EXPECT_FALSE(boost::filesystem::exists(locations[0] + "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1"));
      EXPECT_TRUE(boost::filesystem::exists(locations[1] + "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1"));
      // Remove the second file
      EXPECT_EQ(1, cleanup.BruteForceCleanupOfOldFiles(locations[1], timeFirst, spaceSaved));
      EXPECT_EQ(1, spaceSaved);
      EXPECT_FALSE(boost::filesystem::exists(locations[0] + "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1"));
      EXPECT_FALSE(boost::filesystem::exists(locations[1] + "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1"));

      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(0, stats.aTotalFiles);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
   }
#endif
}

#ifdef LR_DEBUG

TEST_F(DiskCleanupTest, SystemTest_GetPcapStoreUsageSamePartition) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   cleanup.mRealFilesSystemAccess = true;
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup9";
   cleanup.ResetConf();
   Conf& conf = cleanup.GetConf();
   EXPECT_EQ(conf.GetFirstPcapCaptureLocation(), "/tmp/TooMuchPcap/");
   EXPECT_EQ(conf.GetPcapCaptureLocations()[0], "/tmp/TooMuchPcap/");

   EXPECT_EQ(conf.GetProbeLocation(), "/tmp/TooMuchPcap");

   // Same partition: disk usage for pcap is calulcated on the folder itself
   // free and total should be huge since it is on the root partition
   cleanup.GetPcapStoreUsage(stats, MemorySize::KByte);
   size_t spaceToCreateADirectory = 4;
   EXPECT_EQ(stats.pcapDiskInGB.Used, spaceToCreateADirectory); // overhead creating dir

   std::string make1MFileFile = "dd bs=1024 count=1024 if=/dev/zero of=";
   make1MFileFile += testDir.str();
   make1MFileFile += "/1MFile";
   EXPECT_EQ(0, system(make1MFileFile.c_str()));
   DiskUsage usage(testDir.str());
   size_t usedKByte = usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::KByte);
   EXPECT_EQ(usedKByte, 1024 + spaceToCreateADirectory); // including 4: overhead

   cleanup.GetPcapStoreUsage(stats, MemorySize::KByte);
   EXPECT_EQ(stats.pcapDiskInGB.Used, 1024 + spaceToCreateADirectory);
}

TEST_F(DiskCleanupTest, SystemTest_RecalculatePCapDiskUsedSamePartition) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   MockElasticSearch es(false);
   cleanup.mRealFilesSystemAccess = true;
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup9";
   cleanup.ResetConf();
   Conf& conf = cleanup.GetConf();

   EXPECT_EQ(conf.GetFirstPcapCaptureLocation(), "/tmp/TooMuchPcap/");
   EXPECT_EQ(conf.GetPcapCaptureLocations()[0], "/tmp/TooMuchPcap/");
   EXPECT_EQ(conf.GetProbeLocation(), "/tmp/TooMuchPcap");

   // Same partition: disk usage for pcap is calulcated on the folder itself
   // free and total should be huge since it is on the root partition

   cleanup.RecalculatePCapDiskUsed(stats, es);
   EXPECT_EQ(stats.aTotalFiles, 0);
   DiskUsage usage(testDir.str());
   size_t usedMB = usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::MB);
   EXPECT_EQ(usedMB, 0);
   EXPECT_EQ(usedMB, stats.aPcapUsageInMB);

   std::string make10MFile = "dd bs=1024 count=10240 if=/dev/zero of=";
   make10MFile += testDir.str();
   make10MFile += "/10MFile";
   EXPECT_EQ(0, system(make10MFile.c_str()));
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup10"; // file limit is 2
   cleanup.ResetConf();

   cleanup.RecalculatePCapDiskUsed(stats, es); // returns in GB
   EXPECT_EQ(stats.aTotalFiles, 1);
   usedMB = usage.RecursiveFolderDiskUsed("/tmp/TooMuchPcap/", MemorySize::MB);
   EXPECT_EQ(usedMB, 10);
   EXPECT_EQ(usedMB, stats.aPcapUsageInMB);


   cleanup.GetPcapStoreUsage(stats, MemorySize::KByte);
   size_t spaceToCreateADirectory = 4;
   EXPECT_EQ(stats.pcapDiskInGB.Used, 10240 + spaceToCreateADirectory);
}
#endif



#ifdef LR_DEBUG

// Could potentially also be DISABLED
// This test will only run if the access to /home and /tmp are on separate
// partitions.
// 
// Mocking so that 'folders' on the same partion is used is not much of a point
// since it is the actual partition calculations and checks that are important

TEST_F(DiskCleanupTest, DISABLED_SystemTest_RecalculatePCapDiskUsedManyPartitions) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   DiskUsage home("/home/tmp/TooMuchPcap");
   DiskUsage root("/tmp/TooMuchPcap");
   MockElasticSearch es(false);
   if (home.FileSystemID() != root.FileSystemID()) {
      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);
      cleanup.mRealFilesSystemAccess = true;
      cleanup.mUseMockConf = true;
      auto& mockedConf = cleanup.mMockedConf;
      tempFileCreate scopedHome(mockedConf, "/home/tmp/TooMuchPcap");
      tempFileCreate scopedRoot(mockedConf, "/tmp/TooMuchPcap");
      ASSERT_TRUE(scopedHome.Init());
      ASSERT_TRUE(scopedRoot.Init());
      mockedConf.mPCapCaptureLocations.clear();
      mockedConf.mPCapCaptureLocations.push_back(scopedHome.mTestDir.str());
      mockedConf.mPCapCaptureLocations.push_back(scopedRoot.mTestDir.str());

      // Verify setup
      ASSERT_EQ(mockedConf.GetPcapCaptureLocations().size(), 2);
      ASSERT_EQ(mockedConf.GetPcapCaptureLocations()[0], scopedHome.mTestDir.str());
      ASSERT_EQ(mockedConf.GetPcapCaptureLocations()[1], scopedRoot.mTestDir.str());
      EXPECT_EQ(mockedConf.GetFirstPcapCaptureLocation(), scopedHome.mTestDir.str());

      Conf& conf = cleanup.GetConf();
      EXPECT_EQ(conf.GetFirstPcapCaptureLocation(), scopedHome.mTestDir.str());
      EXPECT_EQ(conf.GetPcapCaptureLocations()[0], scopedHome.mTestDir.str());
      EXPECT_EQ(conf.GetPcapCaptureLocations()[1], scopedRoot.mTestDir.str());
      EXPECT_NE(scopedRoot.mTestDir.str(), scopedHome.mTestDir.str());
      EXPECT_EQ(mockedConf.GetProbeLocation(), "/usr/local/probe/");

      // Using 2 different partitions for pcaps
      // Verify that RecalculatePCapDiskUsed is using both partitions
      DiskCleanup::DiskSpace pcapDisk{0, 0, 0};
      size_t used_1_a;
      size_t totalFiles;
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(totalFiles, 0);
      DiskUsage atRoot{scopedRoot.mTestDir.str()};
      DiskUsage atHome{scopedHome.mTestDir.str()};
      auto size = MemorySize::MB;
      // Measure disk usage before we add anything
      size_t usedMB_1 = atHome.DiskUsed(size);
      size_t usedMB_2 = atRoot.DiskUsed(size);
      EXPECT_EQ(usedMB_1 + usedMB_2, used_1_a);

      // Add 1 file of 10MB to the 'home' partition
      std::string make10MFile = "dd bs=1024 count=10240 if=/dev/zero of=";
      make10MFile += scopedHome.mTestDir.str();
      make10MFile += "/10MFile";
      EXPECT_EQ(0, system(make10MFile.c_str()));

      // Verify that the 10MB file was written to first location
      size_t used_1_b;
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(totalFiles, 1);
      DiskCleanup::DiskSpace extraCheck;
      cleanup.GetPcapStoreUsage(stats, size);
      EXPECT_EQ(extraCheck.Used, used_1_b);
      atHome.Update();
      atRoot.Update();
      usedMB_1 = atHome.DiskUsed(size);
      usedMB_2 = atRoot.DiskUsed(size);
      EXPECT_EQ(usedMB_1 + usedMB_2, 10 + used_1_a);
      EXPECT_EQ(usedMB_1 + usedMB_2, used_1_b);


      // Create another 10MB file and save it to the other location
      make10MFile = "dd bs=1024 count=10240 if=/dev/zero of=";
      make10MFile += scopedRoot.mTestDir.str();
      make10MFile += "/10MFile";
      EXPECT_EQ(0, system(make10MFile.c_str()));
      size_t used_1_c;
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(totalFiles, 2);
      atHome.Update();
      atRoot.Update();
      usedMB_1 = atHome.DiskUsed(size);
      usedMB_2 = atRoot.DiskUsed(size);
      EXPECT_EQ(usedMB_1 + usedMB_2, 20 + used_1_a);
      EXPECT_EQ(usedMB_1 + usedMB_2, used_1_c);

      // Verify that both files were actually created 
      // in the separate directories and that they are on separate partitions
      std::string path1 = scopedHome.mTestDir.str();
      std::string path2 = scopedRoot.mTestDir.str();
      path1.append("/10MFile");
      path2.append("/10MFile");

      EXPECT_TRUE(boost::filesystem::exists(path1));
      EXPECT_TRUE(boost::filesystem::exists(path2));
      EXPECT_NE(atHome.FileSystemID(), atRoot.FileSystemID());
   }
}
#endif

TEST_F(DiskCleanupTest, CleanupOldPcapFiles) {
#ifdef LR_DEBUG
   if (geteuid() == 0) {
      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);
      cleanup.mRealFilesSystemAccess = true;

      MockElasticSearch es(false);

      es.mUpdateDocAlwaysPasses = true;
      es.RunQueryGetIdsAlwaysPasses = true;
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup1";
      cleanup.ResetConf();
      cleanup.RecalculatePCapDiskUsed(stats, es);
      ASSERT_FALSE(cleanup.TooMuchPCap(stats));

      std::string makeADir;

      std::string makeSmallFile = "touch ";
      makeSmallFile += testDir.str();
      makeSmallFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1M";
      EXPECT_EQ(0, system(makeSmallFile.c_str())); // 1 file, empty
      cleanup.RecalculatePCapDiskUsed(stats, es);

      std::string make1MFileFile = "dd bs=1024 count=1024 if=/dev/zero of=";
      make1MFileFile += testDir.str();
      make1MFileFile += "/1MFile";
      EXPECT_EQ(0, system(make1MFileFile.c_str())); // 2 files, 1MB
      cleanup.RecalculatePCapDiskUsed(stats, es);

      make1MFileFile = "dd bs=1048575 count=1 if=/dev/zero of="; // 1MB-1byte
      make1MFileFile += testDir.str();
      make1MFileFile += "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1";
      EXPECT_EQ(0, system(make1MFileFile.c_str())); // 3 files, 2MB -1byte (+folder overhead)

      mConf.mConfLocation = "resources/test.yaml.DiskCleanup7";
      cleanup.ResetConf();
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      PathAndFileName element(testDir.str() + "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1M", "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb1M");
      es.mOldestFiles.insert(element);
      stats.canSendStats = false;
      cleanup.CleanupOldPcapFiles(es, stats); // empty file removed, 2 left
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      es.mOldestFiles.clear();
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(2, stats.aTotalFiles);
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup10";
      cleanup.ResetConf();

      Conf& conf = cleanup.GetConf();
      auto fileLimit = conf.GetPcapCaptureFileLimit();
      auto sizeLimit = conf.GetPcapCaptureSizeLimit();
      EXPECT_NE(conf.GetPcapCaptureLocations()[0], conf.GetProbeLocation());
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      std::get<0>(element) = testDir.str() + "/aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1";
      std::get<1>(element) = "aaabbbbbbbbbbbbbbbbbbbbbbbbbbbbb1Mm1";
      es.mOldestFiles.insert(element);
      stats.canSendStats = false;
      cleanup.CleanupOldPcapFiles(es, stats); // 2MB files removed
      es.mOldestFiles.clear();
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      EXPECT_EQ(1, stats.aTotalFiles);
      size_t ByteTotalLeft = 1052672;
      DiskUsage usage(testDir.str());
      EXPECT_EQ(usage.RecursiveFolderDiskUsed(testDir.str(), MemorySize::Byte), ByteTotalLeft);

      mConf.mConfLocation = "resources/test.yaml.DiskCleanup9";
      cleanup.ResetConf();
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aTotalFiles, 1);
      EXPECT_EQ(stats.aPcapUsageInMB, 1);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      std::get<0>(element) = testDir.str() + "/1MFile";
      std::get<1>(element) = "1MFile";
      es.mOldestFiles.insert(element);
      cleanup.CleanupOldPcapFiles(es, stats);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));

   }
#endif
}
#ifdef LR_DEBUG

TEST_F(DiskCleanupTest, ESFailuresGoAheadAndRemoveFiles) {
   if (geteuid() == 0) {
      std::string makeADir;

      MockElasticSearch es(false);
      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);
      std::string makeSmallFile = "touch ";
      makeSmallFile += testDir.str();
      makeSmallFile += "/161122fd-6681-42a3-b953-48beb5247172";
      EXPECT_EQ(0, system(makeSmallFile.c_str()));
      EXPECT_TRUE(boost::filesystem::exists("/tmp/TooMuchPcap/161122fd-6681-42a3-b953-48beb5247172"));
      mConf.mConfLocation = "resources/test.yaml.DiskCleanup9";
      ASSERT_EQ(mConf.GetConf().GetFirstPcapCaptureLocation(), "/tmp/TooMuchPcap/");
      cleanup.ResetConf();
      ASSERT_EQ(cleanup.GetConf().GetFirstPcapCaptureLocation(), "/tmp/TooMuchPcap/");
      ASSERT_EQ(cleanup.GetConf().GetPcapCaptureLocations()[0], "/tmp/TooMuchPcap/");

      std::this_thread::sleep_for(std::chrono::seconds(1));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aTotalFiles, 1);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      es.mBulkUpdateResult = false;
      PathAndFileName element(testDir.str() + "/161122fd-6681-42a3-b953-48beb5247172", "161122fd-6681-42a3-b953-48beb5247172");
      es.mOldestFiles.insert(element);
      size_t filesRemoved;
      size_t spaceSaved;
      time_t oldest;
      EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(1, 2000, es, spaceSaved, oldest, stats));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(boost::filesystem::exists("/tmp/TooMuchPcap/161122fd-6681-42a3-b953-48beb5247172"));
      EXPECT_EQ(stats.aTotalFiles, 0);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));

      EXPECT_EQ(0, system(makeSmallFile.c_str()));
      std::this_thread::sleep_for(std::chrono::seconds(1));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(boost::filesystem::exists("/tmp/TooMuchPcap/161122fd-6681-42a3-b953-48beb5247172"));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aTotalFiles, 1);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      stats.canSendStats = false;
      // the below will do nothing, we already think we've removed that file
      cleanup.CleanupOldPcapFiles(es, stats);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      EXPECT_NE(stats.aTotalFiles, 0);

      // Now it will work, the old cached remove is gone
      cleanup.CleanupOldPcapFiles(es, stats);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      EXPECT_EQ(stats.aTotalFiles, 0);
   }
}

TEST_F(DiskCleanupTest, ESFailuresGoAheadAndRemoveFilesManyLocations) {
   if (geteuid() == 0) {
      std::string makeADir;
      DiskCleanup::DiskSpace probeDisk{0, 0, 0};
      DiskCleanup::DiskSpace pcapDisk{100, 0, 0}; // total, free, used
      MockElasticSearch es(false);

      ProcessClient processClient(mConf.GetConf());
      ASSERT_TRUE(processClient.Initialize());
      MockDiskCleanup cleanup(mConf, processClient);

      // Fake two cleanup locations
      EXPECT_EQ(0, system("mkdir -p /tmp/TooMuchPcap/pcap0"));
      EXPECT_EQ(0, system("mkdir -p /tmp/TooMuchPcap/pcap1"));

      std::string makeFile1 = "touch ";
      std::string makeFile2 = makeFile1;
      std::string file1 = "/tmp/TooMuchPcap/pcap0/161122fd-6681-42a3-b953-48beb5247172";
      std::string file2 = "/tmp/TooMuchPcap/pcap1/161122fd-6681-42a3-b953-48beb5247174";
      makeFile1 += file1;
      makeFile2 += file2;
      EXPECT_EQ(0, system(makeFile1.c_str()));
      EXPECT_EQ(0, system(makeFile2.c_str()));
      EXPECT_TRUE(boost::filesystem::exists(file1));
      EXPECT_TRUE(boost::filesystem::exists(file2));

      mConf.mConfLocation = "resources/test.yaml.GetPcapStoreUsage1";
      EXPECT_EQ(mConf.GetConf().GetFirstPcapCaptureLocation(), "/tmp/TooMuchPcap/pcap0/");
      cleanup.ResetConf();
      EXPECT_EQ(cleanup.GetConf().GetFirstPcapCaptureLocation(), "/tmp/TooMuchPcap/pcap0/");
      auto locations = cleanup.GetConf().GetPcapCaptureLocations();
      ASSERT_EQ(locations.size(), 2);
      EXPECT_EQ(cleanup.GetConf().GetPcapCaptureLocations()[0], "/tmp/TooMuchPcap/pcap0/");
      EXPECT_EQ(cleanup.GetConf().GetPcapCaptureLocations()[1], "/tmp/TooMuchPcap/pcap1/");

      std::this_thread::sleep_for(std::chrono::seconds(1));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aTotalFiles, 2);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      es.mBulkUpdateResult = false;
      PathAndFileName element(file1, "161122fd-6681-42a3-b953-48beb5247172");
      es.mOldestFiles.insert(element);
      std::get<0>(element) = file2;
      es.mOldestFiles.insert(element);
      size_t filesRemoved;
      size_t spaceSaved;
      time_t oldest;
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(boost::filesystem::exists(file1));
      EXPECT_TRUE(boost::filesystem::exists(file2));
      LOG(INFO) << "\n\n^^^^^^^^^^^^^^1^^^^^^^^^^^^^^^^^^^^^^^^^^^^^";
      EXPECT_EQ(0, cleanup.RemoveOldestPCapFilesInES(1, 2000, es, spaceSaved, oldest, stats));
      LOG(INFO) << "\n\n^^^^^^^^^^^^^^^2^^^^^^^^^^^^^^^^^^^^^^^^^^^^";
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(boost::filesystem::exists(file1));
      EXPECT_FALSE(boost::filesystem::exists(file2));
      EXPECT_EQ(stats.aTotalFiles, 0);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));

      // Create two files. One in each directory
      // Verify that cleanup works in both directories
      EXPECT_EQ(0, system(makeFile1.c_str()));
      EXPECT_EQ(0, system(makeFile2.c_str()));
      std::this_thread::sleep_for(std::chrono::seconds(1));
      EXPECT_TRUE(boost::filesystem::exists(file1));
      EXPECT_TRUE(boost::filesystem::exists(file2));
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_EQ(stats.aTotalFiles, 2);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      stats.canSendStats = false;
      cleanup.CleanupOldPcapFiles(es, stats);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_TRUE(cleanup.TooMuchPCap(stats));
      EXPECT_NE(stats.aTotalFiles, 0);
      EXPECT_TRUE(boost::filesystem::exists(file1));
      EXPECT_TRUE(boost::filesystem::exists(file2));

      cleanup.CleanupOldPcapFiles(es, stats);
      cleanup.RecalculatePCapDiskUsed(stats, es);
      EXPECT_FALSE(cleanup.TooMuchPCap(stats));
      EXPECT_EQ(stats.aTotalFiles, 0);
      EXPECT_FALSE(boost::filesystem::exists(file1));
      EXPECT_FALSE(boost::filesystem::exists(file2));
   }
}

TEST_F(DiskCleanupTest, FSMath) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   cleanup.mFleSystemInfo.f_bfree = 100 << B_TO_MB_SHIFT;
   cleanup.mFleSystemInfo.f_frsize = 1024;
   cleanup.mFleSystemInfo.f_blocks = 109 << B_TO_MB_SHIFT;
   cleanup.mFleSystemInfo.f_frsize = 1024;

   cleanup.GetPcapStoreUsage(stats, MemorySize::GB);
   EXPECT_EQ(100, stats.pcapDiskInGB.Free);
   EXPECT_EQ(109, stats.pcapDiskInGB.Total);

   cleanup.mRealFilesSystemAccess = true;
   mConf.mConfLocation = "resources/test.yaml.DiskCleanup1";
   cleanup.ResetConf();
   cleanup.GetPcapStoreUsage(stats, MemorySize::GB);
   EXPECT_NE(0, stats.pcapDiskInGB.Free);
   EXPECT_NE(0, stats.pcapDiskInGB.Total);
}

TEST_F(DiskCleanupTest, SendStats) {
   ProcessClient processClient(mConf.GetConf());
   ASSERT_TRUE(processClient.Initialize());
   MockDiskCleanup cleanup(mConf, processClient);
   size_t valueInByte = 1024 * 4096;
   DiskCleanup::PacketCaptureFilesystemDetails zeroed{0, 0, 0, 0};
   DiskCleanup::PacketCaptureFilesystemDetails detailsWithValues{valueInByte, valueInByte, valueInByte, valueInByte};


   cleanup.mDoPseudoGetUpdatedDiskInfo = true;
   cleanup.mPseudoGetUpdatedDiskInfo = detailsWithValues;

   MockSendStats sendQueue;
   MockElasticSearch es(false);
   es.mMockListOfIndexes.clear();
   es.mFakeIndexList = true;
   size_t ignored = 1;
   std::time_t oldTime = std::time(nullptr);
   std::time_t copyOldTime = oldTime;
   std::this_thread::sleep_for(std::chrono::seconds(6)); // 5s is the limit in DiskCleanup::SendAllStats
   DiskCleanup::DiskSpace ignoredDiskSpace{0, 0, 0};
   stats.canSendStats = true;
   stats.sendQueue = &sendQueue;
   stats.currentTime = oldTime;
   stats.pcapFilesystemDetails = zeroed;
   stats.pcapDiskInGB = ignoredDiskSpace;
   stats.probeDiskInGB = ignoredDiskSpace;
   cleanup.SendAllStats(es, stats);
   ASSERT_EQ(sendQueue.mSendStatKeys.size(), sendQueue.mSendStatValues.size());
   size_t counter = 0;
   for (auto& key : sendQueue.mSendStatKeys) {
      LOG(INFO) << key << ":" << sendQueue.mSendStatValues[counter++];
   }
   ASSERT_TRUE(sendQueue.mSendStatKeys.size() >= 4) << ":" << sendQueue.mSendStatKeys.size();
   EXPECT_EQ(sendQueue.mSendStatKeys[0], "Total_ES_Disk_Writes");
   EXPECT_EQ(sendQueue.mSendStatValues[0], valueInByte);
   EXPECT_EQ(sendQueue.mSendStatKeys[1], "Total_ES_Disk_Reads");
   EXPECT_EQ(sendQueue.mSendStatValues[1], valueInByte);

   EXPECT_EQ(sendQueue.mSendStatKeys[2], "Total_ES_Disk_Mb_Writes");
   EXPECT_EQ(sendQueue.mSendStatValues[2], valueInByte >> B_TO_MB_SHIFT);
   EXPECT_EQ(sendQueue.mSendStatKeys[3], "Total_ES_Disk_Mb_Reads");
   EXPECT_EQ(sendQueue.mSendStatValues[3], valueInByte >> B_TO_MB_SHIFT);


   // Now using weird values for force calculations to become negative
   // We should then just send zero values
   DiskCleanup::PacketCaptureFilesystemDetails zeroed2{0, 0, 0, 0};
   cleanup.mPseudoGetUpdatedDiskInfo = zeroed2; // no values as current
   sendQueue.mSendStatKeys.clear();
   sendQueue.mSendStatValues.clear();
   // send values as current but latest values are zeroes we get negative

   stats.pcapFilesystemDetails = detailsWithValues;
   stats.currentTime = copyOldTime;
   cleanup.SendAllStats(es, stats);
   ASSERT_EQ(sendQueue.mSendStatKeys.size(), sendQueue.mSendStatValues.size());
   counter = 0;
   for (auto& key : sendQueue.mSendStatKeys) {
      LOG(INFO) << key << ":" << sendQueue.mSendStatValues[counter++];
   }
   ASSERT_TRUE(sendQueue.mSendStatKeys.size() >= 4) << ":" << sendQueue.mSendStatKeys.size();
   EXPECT_EQ(sendQueue.mSendStatKeys[0], "Total_ES_Disk_Writes");
   EXPECT_EQ(sendQueue.mSendStatValues[0], 0);
   EXPECT_EQ(sendQueue.mSendStatKeys[1], "Total_ES_Disk_Reads");
   EXPECT_EQ(sendQueue.mSendStatValues[1], 0);

   EXPECT_EQ(sendQueue.mSendStatKeys[2], "Total_ES_Disk_Mb_Writes");
   EXPECT_EQ(sendQueue.mSendStatValues[2], 0);
   EXPECT_EQ(sendQueue.mSendStatKeys[3], "Total_ES_Disk_Mb_Reads");
   EXPECT_EQ(sendQueue.mSendStatValues[3], 0);
}



#endif
