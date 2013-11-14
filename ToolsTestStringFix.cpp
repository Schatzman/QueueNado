/* 
 * File:   ToolsStringFix.cpp
 * Author: kjell
 * 
 * Created on August 2, 2013, 3:18 PM
 */

#include "ToolsTestStringFix.h"
#include "StringFix.h"
#include "FileIO.h"
#include <g2log.hpp>
#include <string>
#include <vector>

TEST_F(ToolsTestStringFix, NoTrim) {
   const std::string noSpaceAtEnd{"abcde efgh"};
   auto notTrimmed = stringfix::trim(noSpaceAtEnd);
   ASSERT_EQ(notTrimmed, noSpaceAtEnd);
}

TEST_F(ToolsTestStringFix, TrimEnds) {
   const std::string spaceAtEnd{"\n\t abcde efgh\n\t "};
   const std::string noSpaceAtEnd{"abcde efgh"};
   auto trimmed = stringfix::trim(noSpaceAtEnd, {"\n\t "});
   
   ASSERT_NE(trimmed, spaceAtEnd);
   ASSERT_EQ(trimmed, noSpaceAtEnd);
}


TEST_F(ToolsTestStringFix, NoSplit) {
   const std::string numbers{"1,2,3,4,5,6"};
   auto tokens = stringfix::split("", numbers);
   ASSERT_EQ(tokens.size(), 1);
   ASSERT_EQ(tokens[0], numbers);
}

TEST_F(ToolsTestStringFix, Split) {
   const std::string numbers{"1,2,3,4,5,6"};
   auto tokens = stringfix::split(",", numbers);
   ASSERT_EQ(tokens.size(), 6);
   size_t count = 1;
   for(auto t : tokens) {
      ASSERT_EQ(t, std::to_string(count++));
   }
}

TEST_F(ToolsTestStringFix, SplitAdvanced) {
   const std::string numbers{"1 2,3 4,5 6"};
   auto tokens = stringfix::split(", ", numbers);
   ASSERT_EQ(tokens.size(), 6);
   size_t count = 1;
   for(auto t : tokens) {
      ASSERT_EQ(t, std::to_string(count++));
   }
}


TEST_F(ToolsTestStringFix, EmptyStringAsKey__ExpectingZeroExplode) {
   const std::string greeting{"Hello World! Hola El Mundo!"};
   auto tokens = stringfix::explode("", greeting);
   ASSERT_EQ(tokens.size(), 1); 
   EXPECT_EQ(tokens[0], greeting); // same returned. no match
}

TEST_F(ToolsTestStringFix, TooBigStringAsKey__ExpectingZeroExplode) {
   const std::string greeting{"Hello World! Hola El Mundo!"};
   const std::string key = greeting+greeting;
   auto tokens = stringfix::explode(key, greeting);
   ASSERT_EQ(tokens.size(), 1); 
   EXPECT_EQ(tokens[0], greeting); // same returned. no match
}



TEST_F(ToolsTestStringFix, NoMatch__ExpectingZeroExplode) {
   const std::string greeting{"Hello World! Hola El Mundo!"};
   auto tokens = stringfix::explode("Goodbye World!", greeting);
   ASSERT_EQ(tokens.size(), 1); // same returned. no match
   EXPECT_EQ(tokens[0], greeting); // same returned. no match
}

TEST_F(ToolsTestStringFix, ExplodeWithCompleteMatch__ExpectingZeroReturn__IeFullExplode) {
   const std::string greeting{"Hello World! Hola El Mundo!"};
   auto tokens = stringfix::explode(greeting, greeting);
   EXPECT_EQ(tokens.size(), 0);
}

TEST_F(ToolsTestStringFix, ExplodeWithOneCharacterMatch__ExpectingTwoReturns) {
   const std::string greeting{"Hello World! Hola El Mundo!"};
   auto tokens = stringfix::explode("!", greeting);
   ASSERT_EQ(tokens.size(), 2);
   EXPECT_EQ(tokens[0], "Hello World");
   EXPECT_EQ(tokens[1], " Hola El Mundo");
}


TEST_F(ToolsTestStringFix, RealWorldExample_DAS_Matching) {
   auto partedReading = FileIO::ReadAsciiFileContent("resources/parted.119.das.txt");
   ASSERT_FALSE(partedReading.HasFailed());
   std::string parted = partedReading.result;
   LOG(DEBUG) << "read parted information: \n" << parted;

   std::string firstItem = {"/dev/sda:299GB:scsi:512:512:msdos:DELL PERC H710;\n1:1049kB:525MB:524MB:ext4::boot;\n2:525MB:299GB:299GB:::lvm;"};
   std::string lastItem = {"/dev/mapper/vg_probe00-lv_root:211GB:dm:512:512:loop:Linux device-mapper (linear);\n1:0.00B:211GB:211GB:ext4::;"};
   std::vector<std::string> tokens = stringfix::explode("BYT;", parted);

   ASSERT_EQ(tokens.size(), 7);
   EXPECT_EQ(stringfix::trim(tokens[0], "\n \t"), firstItem);
   EXPECT_EQ(stringfix::trim(tokens[6], "\n \t"), lastItem);
}
