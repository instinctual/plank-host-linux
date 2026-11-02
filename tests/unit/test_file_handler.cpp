/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Test src/file_handler.*.
 */
#include "../tests_common.h"

#include <format>
#include <src/file_handler.h>

struct FileHandlerParentDirectoryTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(FileHandlerParentDirectoryTest, Run) {
  auto [input, expected] = GetParam();
  EXPECT_EQ(file_handler::get_parent_directory(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerParentDirectoryTest,
  testing::Values(
    std::make_tuple("/path/to/file.txt", "/path/to"),
    std::make_tuple("/path/to/directory", "/path/to"),
    std::make_tuple("/path/to/directory/", "/path/to")
  )
);

struct FileHandlerMakeDirectoryTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, bool, bool>> {};

TEST_P(FileHandlerMakeDirectoryTest, Run) {
  auto [input, expected, remove] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/path/";
  input = test_dir + input;

  EXPECT_EQ(file_handler::make_directory(input), expected);
  EXPECT_TRUE(std::filesystem::exists(input));

  // remove test directory
  if (remove) {
    std::filesystem::remove_all(test_dir);
    EXPECT_FALSE(std::filesystem::exists(test_dir));
  }
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerMakeDirectoryTest,
  testing::Values(
    std::make_tuple("dir_123", true, false),
    std::make_tuple("dir_123", true, true),
    std::make_tuple("dir_123/abc", true, false),
    std::make_tuple("dir_123/abc", true, true)
  )
);

struct FileHandlerTests: BaseTest, testing::WithParamInterface<std::tuple<int, std::string>> {};

INSTANTIATE_TEST_SUITE_P(
  TestFiles,
  FileHandlerTests,
  testing::Values(
    std::make_tuple(0, ""),  // empty file
    std::make_tuple(1, "a"),  // single character
    std::make_tuple(2, "Mr. Blue Sky - Electric Light Orchestra"),  // single line
    std::make_tuple(3, R"(
Morning! Today's forecast calls for blue skies
The sun is shining in the sky
There ain't a cloud in sight
It's stopped raining
Everybody's in the play
And don't you know, it's a beautiful new day
Hey, hey, hey!
Running down the avenue
See how the sun shines brightly in the city
All the streets where once was pity
Mr. Blue Sky is living here today!
Hey, hey, hey!
    )")  // multi-line
  )
);

TEST_P(FileHandlerTests, WriteFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = std::format("write_file_test_{}.txt", fileNum);
  EXPECT_EQ(
    file_handler::write_file(
      fileName.c_str(),
      content,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
    0);
}

TEST_P(FileHandlerTests, ReadFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = std::format("write_file_test_{}.txt", fileNum);
  EXPECT_EQ(file_handler::read_file(fileName.c_str()), content);
}

TEST(FileHandlerTests, ReadMissingFileTest) {
  // read missing file
  EXPECT_EQ(file_handler::read_file("non-existing-file.txt"), "");
}

TEST(FileHandlerTests, WriteFileSetsPermissionsAtCreation) {
  const std::string file_name = "private_write_file_test.txt";
  const auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;

  ASSERT_EQ(file_handler::write_file(file_name.c_str(), "secret", permissions), 0);
  EXPECT_EQ(std::filesystem::status(file_name).permissions() & std::filesystem::perms::all, permissions);
  std::filesystem::remove(file_name);
}

TEST(FileHandlerTests, WriteFileRejectsSymbolicLinks) {
  const std::string target_name = "private_write_file_target.txt";
  const std::string link_name = "private_write_file_link.txt";
  const auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;

  ASSERT_EQ(file_handler::write_file(target_name.c_str(), "unchanged", permissions), 0);
  std::filesystem::create_symlink(target_name, link_name);
  ASSERT_TRUE(std::filesystem::is_symlink(link_name));
  EXPECT_EQ(file_handler::write_file(link_name.c_str(), "replaced", permissions), -1);
  EXPECT_EQ(file_handler::read_file(target_name.c_str()), "unchanged");

  std::filesystem::remove(link_name);
  std::filesystem::remove(target_name);
}
