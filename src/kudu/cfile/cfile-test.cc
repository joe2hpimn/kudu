// Copyright (c) 2012, Cloudera, inc
// Confidential Cloudera Information: Covered by NDA.

#include <gtest/gtest.h>
#include <glog/logging.h>
#include <stdlib.h>

#include <boost/assign/list_of.hpp>
#include "kudu/cfile/cfile-test-base.h"
#include "kudu/cfile/cfile_reader.h"
#include "kudu/cfile/cfile_writer.h"
#include "kudu/cfile/cfile.pb.h"
#include "kudu/cfile/index_block.h"
#include "kudu/cfile/index_btree.h"
#include "kudu/common/columnblock.h"
#include "kudu/fs/fs-test-util.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/stringprintf.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/stopwatch.h"

DECLARE_string(cfile_do_on_finish);

namespace kudu {
namespace cfile {

using fs::CountingReadableBlock;
using fs::ReadableBlock;
using fs::WritableBlock;

class TestCFile : public CFileTestBase {
 protected:
  template <class DataGeneratorType>
  void TestReadWriteFixedSizeTypes(EncodingType encoding) {
    BlockId block_id;
    DataGeneratorType generator;
    WriteTestFile(&generator, encoding, NO_COMPRESSION, 10000, SMALL_BLOCKSIZE, &block_id);

    gscoped_ptr<ReadableBlock> block;
    ASSERT_OK(fs_manager_->OpenBlock(block_id, &block));
    gscoped_ptr<CFileReader> reader;
    ASSERT_OK(CFileReader::Open(block.Pass(), ReaderOptions(), &reader));

    BlockPointer ptr;

    gscoped_ptr<CFileIterator> iter;
    ASSERT_OK(reader->NewIterator(&iter, CFileReader::CACHE_BLOCK));

    ASSERT_OK(iter->SeekToOrdinal(5000));
    ASSERT_EQ(5000u, iter->GetCurrentOrdinal());

    // Seek to last key exactly, should succeed
    ASSERT_OK(iter->SeekToOrdinal(9999));
    ASSERT_EQ(9999u, iter->GetCurrentOrdinal());

    // Seek to after last key. Should result in not found.
    ASSERT_TRUE(iter->SeekToOrdinal(10000).IsNotFound());

    // Seek to start of file
    ASSERT_OK(iter->SeekToOrdinal(0));
    ASSERT_EQ(0u, iter->GetCurrentOrdinal());

    // Fetch all data.
    ScopedColumnBlock<DataGeneratorType::kDataType> out(10000);
    size_t n = 10000;
    ASSERT_OK(iter->CopyNextValues(&n, &out));
    ASSERT_EQ(10000, n);

    DataGeneratorType data_generator_pre;

    for (int i = 0; i < 10000; i++) {
      if (out[i] != data_generator_pre.BuildTestValue(0,i)) {
        FAIL() << "mismatch at index " << i
               << " expected: " << data_generator_pre.BuildTestValue(0,i)
               << " got: " << out[i];
      }
      out[i] = 0;
    }

    // Fetch all data using small batches of only a few rows.
    // This should catch edge conditions like a batch lining up exactly
    // with the end of a block.
    unsigned int seed = time(NULL);
    LOG(INFO) << "Using random seed: " << seed;
    srand(seed);
    ASSERT_OK(iter->SeekToOrdinal(0));
    size_t fetched = 0;
    while (fetched < 10000) {
      ColumnBlock advancing_block(out.type_info(), NULL,
                                  out.data() + (fetched * out.stride()),
                                  out.nrows() - fetched, out.arena());
      ASSERT_TRUE(iter->HasNext());
      size_t batch_size = random() % 5 + 1;
      size_t n = batch_size;
      ASSERT_OK(iter->CopyNextValues(&n, &advancing_block));
      ASSERT_LE(n, batch_size);
      fetched += n;
    }
    ASSERT_FALSE(iter->HasNext());

    DataGeneratorType data_generator_post;

    // Re-verify
    for (int i = 0; i < 10000; i++) {
      if (out[i] != data_generator_post.BuildTestValue(0,i)) {
        FAIL() << "mismatch at index " << i
               << " expected: " << data_generator_post.BuildTestValue(0,i)
               << " got: " << out[i];
      }
      out[i] = 0;
    }

    TimeReadFile(fs_manager_.get(), block_id, &n);
    ASSERT_EQ(10000, n);
  }

  template <class DataGeneratorType>
  void TimeSeekAndReadFileWithNulls(DataGeneratorType* generator,
                                    const BlockId& block_id, size_t num_entries) {
    gscoped_ptr<ReadableBlock> block;
    ASSERT_OK(fs_manager_->OpenBlock(block_id, &block));
    gscoped_ptr<CFileReader> reader;
    ASSERT_OK(CFileReader::Open(block.Pass(), ReaderOptions(), &reader));
    ASSERT_EQ(DataGeneratorType::kDataType, reader->data_type());

    gscoped_ptr<CFileIterator> iter;
    ASSERT_OK(reader->NewIterator(&iter, CFileReader::CACHE_BLOCK));

    Arena arena(8192, 8*1024*1024);
    ScopedColumnBlock<DataGeneratorType::kDataType> cb(10);

    const int kNumLoops = AllowSlowTests() ? num_entries : 10;
    for (int loop = 0; loop < kNumLoops; loop++) {
      // Seek to a random point in the file,
      // or just try each entry as starting point if you're running SlowTests
      int target = AllowSlowTests() ? loop : (random() % (num_entries - 1));
      SCOPED_TRACE(target);
      ASSERT_OK(iter->SeekToOrdinal(target));
      ASSERT_TRUE(iter->HasNext());

      // Read and verify several ColumnBlocks from this point in the file.
      int read_offset = target;
      for (int block = 0; block < 3 && iter->HasNext(); block++) {
        SCOPED_TRACE(block);
        size_t n = cb.nrows();
        ASSERT_OK_FAST(iter->CopyNextValues(&n, &cb));
        ASSERT_EQ(n, std::min(num_entries - read_offset, cb.nrows()));

        // Verify that the block data is correct.
        generator->Build(read_offset, n);
        for (size_t j = 0; j < n; ++j) {
          SCOPED_TRACE(j);
          bool expected_null = generator->TestValueShouldBeNull(read_offset + j);
          ASSERT_EQ(expected_null, cb.is_null(j));
          if (!expected_null) {
            ASSERT_EQ((*generator)[j], cb[j]);
          }
        }
        cb.arena()->Reset();
        read_offset += n;
      }
    }
  }

  template <class DataGeneratorType>
  void TestNullTypes(DataGeneratorType* generator, EncodingType encoding,
                     CompressionType compression) {
    BlockId block_id;
    WriteTestFile(generator, encoding, compression, 10000, SMALL_BLOCKSIZE, &block_id);

    size_t n;
    TimeReadFile(fs_manager_.get(), block_id, &n);
    ASSERT_EQ(n, 10000);

    generator->Reset();
    TimeSeekAndReadFileWithNulls(generator, block_id, n);
  }


  void TestReadWriteRawBlocks(CompressionType compression, int num_entries) {
    // Test Write
    gscoped_ptr<WritableBlock> sink;
    ASSERT_OK(fs_manager_->CreateNewBlock(&sink));
    BlockId id = sink->id();
    WriterOptions opts;
    opts.write_posidx = true;
    opts.write_validx = false;
    opts.block_size = FLAGS_cfile_test_block_size;
    opts.storage_attributes = ColumnStorageAttributes(PLAIN_ENCODING, compression);
    CFileWriter w(opts, STRING, false, sink.Pass());
    ASSERT_OK(w.Start());
    for (uint32_t i = 0; i < num_entries; i++) {
      vector<Slice> slices;
      slices.push_back(Slice("Head"));
      slices.push_back(Slice("Body"));
      slices.push_back(Slice("Tail"));
      slices.push_back(Slice(reinterpret_cast<uint8_t *>(&i), 4));
      ASSERT_OK(w.AppendRawBlock(slices, i, NULL, "raw-data"));
    }
    ASSERT_OK(w.Finish());

    // Test Read
    gscoped_ptr<ReadableBlock> source;
    ASSERT_OK(fs_manager_->OpenBlock(id, &source));
    gscoped_ptr<CFileReader> reader;
    ASSERT_OK(CFileReader::Open(source.Pass(), ReaderOptions(), &reader));

    gscoped_ptr<IndexTreeIterator> iter;
    iter.reset(IndexTreeIterator::Create(reader.get(), UINT32, reader->posidx_root()));
    ASSERT_OK(iter->SeekToFirst());

    uint8_t data[16];
    Slice expected_data(data, 16);
    memcpy(data, "HeadBodyTail", 12);

    uint32_t count = 0;
    do {
      BlockHandle dblk_data;
      BlockPointer blk_ptr = iter->GetCurrentBlockPointer();
      ASSERT_OK(reader->ReadBlock(blk_ptr, CFileReader::CACHE_BLOCK, &dblk_data));

      memcpy(data + 12, &count, 4);
      ASSERT_EQ(expected_data, dblk_data.data());

      count++;
    } while (iter->Next().ok());
    ASSERT_EQ(num_entries, count);
  }

  void TestReadWriteStrings(EncodingType encoding);

#ifdef NDEBUG
  void TestWrite100MFileStrings(EncodingType encoding) {
    BlockId block_id;
    LOG_TIMING(INFO, "writing 100M strings") {
      LOG(INFO) << "Starting writefile";
      StringDataGenerator<false> generator("hello %zu");
      WriteTestFile(&generator, PREFIX_ENCODING, NO_COMPRESSION, 100000000, NO_FLAGS, &block_id);
      LOG(INFO) << "Done writing";
    }

    LOG_TIMING(INFO, "reading 100M strings") {
      LOG(INFO) << "Starting readfile";
      size_t n;
      TimeReadFile(fs_manager_.get(), block_id, &n);
      ASSERT_EQ(100000000, n);
      LOG(INFO) << "End readfile";
    }
  }
#endif

};


template<DataType type>
void CopyOne(CFileIterator *it,
             typename TypeTraits<type>::cpp_type *ret,
             Arena *arena) {
  ColumnBlock cb(GetTypeInfo(type), NULL, ret, 1, arena);
  size_t n = 1;
  ASSERT_OK(it->CopyNextValues(&n, &cb));
  ASSERT_EQ(1, n);
}

#ifdef NDEBUG
// Only run the 100M entry tests in non-debug mode.
// They take way too long with debugging enabled.

TEST_F(TestCFile, TestWrite100MFileInts) {
  BlockId block_id;
  LOG_TIMING(INFO, "writing 100m ints") {
    LOG(INFO) << "Starting writefile";
    UInt32DataGenerator<false> generator;
    WriteTestFile(&generator, GROUP_VARINT, NO_COMPRESSION, 100000000, NO_FLAGS, &block_id);
    LOG(INFO) << "Done writing";
  }

  LOG_TIMING(INFO, "reading 100M ints") {
    LOG(INFO) << "Starting readfile";
    size_t n;
    TimeReadFile(fs_manager_.get(), block_id, &n);
    ASSERT_EQ(100000000, n);
    LOG(INFO) << "End readfile";
  }
}

TEST_F(TestCFile, TestWrite100MFileNullableInts) {
  BlockId block_id;
  LOG_TIMING(INFO, "writing 100m nullable ints") {
    LOG(INFO) << "Starting writefile";
    UInt32DataGenerator<true> generator;
    WriteTestFile(&generator, PLAIN_ENCODING, NO_COMPRESSION, 100000000, NO_FLAGS, &block_id);
    LOG(INFO) << "Done writing";
  }

  LOG_TIMING(INFO, "reading 100M nullable ints") {
    LOG(INFO) << "Starting readfile";
    size_t n;
    TimeReadFile(fs_manager_.get(), block_id, &n);
    ASSERT_EQ(100000000, n);
    LOG(INFO) << "End readfile";
  }
}

TEST_F(TestCFile, TestWrite100MFileStringsPrefixEncoding) {
  TestWrite100MFileStrings(PREFIX_ENCODING);
}

TEST_F(TestCFile, TestWrite100MFileStringsDictncoding) {
  TestWrite100MFileStrings(DICT_ENCODING);
}

#endif

// Write and Read 1 million unique strings with dictionary encoding
TEST_F(TestCFile, TestWrite1MUniqueFileStringsDictEncoding) {
  BlockId block_id;
  LOG_TIMING(INFO, "writing 1M unique strings") {
    LOG(INFO) << "Starting writefile";
    StringDataGenerator<false> generator("hello %zu");
    WriteTestFile(&generator, DICT_ENCODING, NO_COMPRESSION, 1000000, NO_FLAGS, &block_id);
    LOG(INFO) << "Done writing";
  }

  LOG_TIMING(INFO, "reading 1M strings") {
    LOG(INFO) << "Starting readfile";
    size_t n;
    TimeReadFile(fs_manager_.get(), block_id, &n);
    ASSERT_EQ(1000000, n);
    LOG(INFO) << "End readfile";
  }
}

// Write and Read 1 million strings, which contains duplicates with dictionary encoding
TEST_F(TestCFile, TestWrite1MDuplicateFileStringsDictEncoding) {
  BlockId block_id;
  LOG_TIMING(INFO, "writing 1M duplicate strings") {
    LOG(INFO) << "Starting writefile";

    // The second parameter specify how many distinct strings are there
    DuplicateStringDataGenerator<false> generator("hello %zu", 256);
    WriteTestFile(&generator, DICT_ENCODING, NO_COMPRESSION, 1000000, NO_FLAGS, &block_id);
    LOG(INFO) << "Done writing";
  }

  LOG_TIMING(INFO, "reading 1M strings") {
    LOG(INFO) << "Starting readfile";
    size_t n;
    TimeReadFile(fs_manager_.get(), block_id, &n);
    ASSERT_EQ(1000000, n);
    LOG(INFO) << "End readfile";
  }
}

TEST_F(TestCFile, TestFixedSizeReadWriteUInt32) {
  TestReadWriteFixedSizeTypes<UInt32DataGenerator<false> >(GROUP_VARINT);
  TestReadWriteFixedSizeTypes<UInt32DataGenerator<false> >(PLAIN_ENCODING);
}

TEST_F(TestCFile, TestFixedSizeReadWriteInt32) {
  TestReadWriteFixedSizeTypes<Int32DataGenerator<false> >(PLAIN_ENCODING);
}

TEST_F(TestCFile, TestFixedSizeReadWriteFloat) {
  TestReadWriteFixedSizeTypes<FPDataGenerator<FLOAT, false> >(PLAIN_ENCODING);
}
TEST_F(TestCFile, TestFixedSizeReadWriteDouble) {
  TestReadWriteFixedSizeTypes<FPDataGenerator<DOUBLE, false> >(PLAIN_ENCODING);
}

void EncodeStringKey(const Schema &schema, const Slice& key,
                     gscoped_ptr<EncodedKey> *encoded_key) {
  EncodedKeyBuilder kb(&schema);
  kb.AddColumnKey(&key);
  encoded_key->reset(kb.BuildEncodedKey());
}

void TestCFile::TestReadWriteStrings(EncodingType encoding) {
  Schema schema(boost::assign::list_of
                (ColumnSchema("key", STRING)),
                1);

  const int nrows = 10000;
  BlockId block_id;
  StringDataGenerator<false> generator("hello %04d");
  WriteTestFile(&generator, encoding, NO_COMPRESSION, nrows,
                SMALL_BLOCKSIZE | WRITE_VALIDX, &block_id);

  gscoped_ptr<ReadableBlock> block;
  ASSERT_OK(fs_manager_->OpenBlock(block_id, &block));
  gscoped_ptr<CFileReader> reader;
  ASSERT_OK(CFileReader::Open(block.Pass(), ReaderOptions(), &reader));

  rowid_t reader_nrows;
  ASSERT_OK(reader->CountRows(&reader_nrows));
  ASSERT_EQ(nrows, reader_nrows);

  BlockPointer ptr;

  gscoped_ptr<CFileIterator> iter;
  ASSERT_OK(reader->NewIterator(&iter, CFileReader::CACHE_BLOCK));

  Arena arena(1024, 1024*1024);

  ASSERT_OK(iter->SeekToOrdinal(5000));
  ASSERT_EQ(5000u, iter->GetCurrentOrdinal());
  Slice s;

  CopyOne<STRING>(iter.get(), &s, &arena);
  ASSERT_EQ(string("hello 5000"), s.ToString());

  // Seek to last key exactly, should succeed
  ASSERT_OK(iter->SeekToOrdinal(9999));
  ASSERT_EQ(9999u, iter->GetCurrentOrdinal());

  // Seek to after last key. Should result in not found.
  ASSERT_TRUE(iter->SeekToOrdinal(10000).IsNotFound());


  ////////
  // Now try some seeks by the value instead of position
  /////////

  gscoped_ptr<EncodedKey> encoded_key;
  bool exact;
  s = "hello 5000.5";
  EncodeStringKey(schema, s, &encoded_key);
  ASSERT_OK(iter->SeekAtOrAfter(*encoded_key, &exact));
  ASSERT_FALSE(exact);
  ASSERT_EQ(5001u, iter->GetCurrentOrdinal());
  CopyOne<STRING>(iter.get(), &s, &arena);
  ASSERT_EQ(string("hello 5001"), s.ToString());

  s = "hello 9000";
  EncodeStringKey(schema, s, &encoded_key);
  ASSERT_OK(iter->SeekAtOrAfter(*encoded_key, &exact));
  ASSERT_TRUE(exact);
  ASSERT_EQ(9000u, iter->GetCurrentOrdinal());
  CopyOne<STRING>(iter.get(), &s, &arena);
  ASSERT_EQ(string("hello 9000"), s.ToString());

  // after last entry
  s = "hello 9999x";
  EncodeStringKey(schema, s, &encoded_key);
  EXPECT_TRUE(iter->SeekAtOrAfter(*encoded_key, &exact).IsNotFound());

  // before first entry
  s = "hello";
  EncodeStringKey(schema, s, &encoded_key);
  ASSERT_OK(iter->SeekAtOrAfter(*encoded_key, &exact));
  ASSERT_FALSE(exact);
  ASSERT_EQ(0u, iter->GetCurrentOrdinal());
  CopyOne<STRING>(iter.get(), &s, &arena);
  ASSERT_EQ(string("hello 0000"), s.ToString());

  // to last entry
  s = "hello 9999";
  EncodeStringKey(schema, s, &encoded_key);
  ASSERT_OK(iter->SeekAtOrAfter(*encoded_key, &exact));
  ASSERT_TRUE(exact);
  ASSERT_EQ(9999u, iter->GetCurrentOrdinal());
  CopyOne<STRING>(iter.get(), &s, &arena);
  ASSERT_EQ(string("hello 9999"), s.ToString());

  // Seek to start of file
  ASSERT_OK(iter->SeekToFirst());
  ASSERT_EQ(0u, iter->GetCurrentOrdinal());
  CopyOne<STRING>(iter.get(), &s, &arena);
  ASSERT_EQ(string("hello 0000"), s.ToString());

  // Reseek to start and fetch all data.
  ASSERT_OK(iter->SeekToFirst());

  ScopedColumnBlock<STRING> cb(10000);
  size_t n = 10000;
  ASSERT_OK(iter->CopyNextValues(&n, &cb));
  ASSERT_EQ(10000, n);
}


TEST_F(TestCFile, TestReadWriteStringsPrefixEncoding) {
  TestReadWriteStrings(PREFIX_ENCODING);
}

// Read/Write test for dictionary encoded blocks
TEST_F(TestCFile, TestReadWriteStringsDictEncoding) {
  TestReadWriteStrings(DICT_ENCODING);
}

// Test that metadata entries stored in the cfile are persisted.
TEST_F(TestCFile, TestMetadata) {
  BlockId block_id;

  // Write the file.
  {
    gscoped_ptr<WritableBlock> sink;
    ASSERT_OK(fs_manager_->CreateNewBlock(&sink));
    block_id = sink->id();
    WriterOptions opts;
    opts.storage_attributes = ColumnStorageAttributes(GROUP_VARINT);
    CFileWriter w(opts, UINT32, false, sink.Pass());

    w.AddMetadataPair("key_in_header", "header value");
    ASSERT_OK(w.Start());

    uint32_t val = 1;
    ASSERT_OK(w.AppendEntries(&val, 1));

    w.AddMetadataPair("key_in_footer", "footer value");
    ASSERT_OK(w.Finish());
  }

  // Read the file and ensure metadata is present.
  {
    gscoped_ptr<ReadableBlock> source;
    ASSERT_OK(fs_manager_->OpenBlock(block_id, &source));
    gscoped_ptr<CFileReader> reader;
    ASSERT_OK(CFileReader::Open(source.Pass(), ReaderOptions(), &reader));
    string val;
    ASSERT_TRUE(reader->GetMetadataEntry("key_in_header", &val));
    ASSERT_EQ(val, "header value");
    ASSERT_TRUE(reader->GetMetadataEntry("key_in_footer", &val));
    ASSERT_EQ(val, "footer value");
    ASSERT_FALSE(reader->GetMetadataEntry("not a key", &val));
  }
}

TEST_F(TestCFile, TestDefaultColumnIter) {
  const int kNumItems = 64;
  uint8_t null_bitmap[BitmapSize(kNumItems)];
  uint32_t data[kNumItems];

  // Test Int Default Value
  uint32_t int_value = 15;
  DefaultColumnValueIterator iter(UINT32, &int_value);
  ColumnBlock int_col(GetTypeInfo(UINT32), NULL, data, kNumItems, NULL);
  ASSERT_OK(iter.Scan(&int_col));
  for (size_t i = 0; i < int_col.nrows(); ++i) {
    ASSERT_EQ(int_value, *reinterpret_cast<const uint32_t *>(int_col.cell_ptr(i)));
  }

  // Test Int Nullable Default Value
  int_value = 321;
  DefaultColumnValueIterator nullable_iter(UINT32, &int_value);
  ColumnBlock nullable_col(GetTypeInfo(UINT32), null_bitmap, data, kNumItems, NULL);
  ASSERT_OK(nullable_iter.Scan(&nullable_col));
  for (size_t i = 0; i < nullable_col.nrows(); ++i) {
    ASSERT_FALSE(nullable_col.is_null(i));
    ASSERT_EQ(int_value, *reinterpret_cast<const uint32_t *>(nullable_col.cell_ptr(i)));
  }

  // Test NULL Default Value
  DefaultColumnValueIterator null_iter(UINT32,  NULL);
  ColumnBlock null_col(GetTypeInfo(UINT32), null_bitmap, data, kNumItems, NULL);
  ASSERT_OK(null_iter.Scan(&null_col));
  for (size_t i = 0; i < null_col.nrows(); ++i) {
    ASSERT_TRUE(null_col.is_null(i));
  }

  // Test String Default Value
  Slice str_data[kNumItems];
  Slice str_value("Hello");
  Arena arena(32*1024, 256*1024);
  DefaultColumnValueIterator str_iter(STRING, &str_value);
  ColumnBlock str_col(GetTypeInfo(STRING), NULL, str_data, kNumItems, &arena);
  ASSERT_OK(str_iter.Scan(&str_col));
  for (size_t i = 0; i < str_col.nrows(); ++i) {
    ASSERT_EQ(str_value, *reinterpret_cast<const Slice *>(str_col.cell_ptr(i)));
  }
}

TEST_F(TestCFile, TestAppendRaw) {
  TestReadWriteRawBlocks(NO_COMPRESSION, 1000);
  TestReadWriteRawBlocks(SNAPPY, 1000);
  TestReadWriteRawBlocks(LZ4, 1000);
  TestReadWriteRawBlocks(ZLIB, 1000);
}

TEST_F(TestCFile, TestNullInts) {
  UInt32DataGenerator<true> generator;
  TestNullTypes(&generator, GROUP_VARINT, NO_COMPRESSION);
  TestNullTypes(&generator, GROUP_VARINT, LZ4);
}

TEST_F(TestCFile, TestNullFloats) {
  FPDataGenerator<FLOAT, true> generator;
  TestNullTypes(&generator, PLAIN_ENCODING, NO_COMPRESSION);
}

TEST_F(TestCFile, TestNullPrefixStrings) {
  StringDataGenerator<true> generator("hello %zu");
  TestNullTypes(&generator, PLAIN_ENCODING, NO_COMPRESSION);
  TestNullTypes(&generator, PLAIN_ENCODING, LZ4);
}

TEST_F(TestCFile, TestNullPlainStrings) {
  StringDataGenerator<true> generator("hello %zu");
  TestNullTypes(&generator, PREFIX_ENCODING, NO_COMPRESSION);
  TestNullTypes(&generator, PREFIX_ENCODING, LZ4);
}

// Test for dictionary encoding
TEST_F(TestCFile, TestNullDictStrings) {
  StringDataGenerator<true> generator("hello %zu");
  TestNullTypes(&generator, DICT_ENCODING, NO_COMPRESSION);
  TestNullTypes(&generator, DICT_ENCODING, LZ4);
}

TEST_F(TestCFile, TestReleaseBlock) {
  gscoped_ptr<WritableBlock> sink;
  ASSERT_OK(fs_manager_->CreateNewBlock(&sink));
  ASSERT_EQ(WritableBlock::CLEAN, sink->state());
  BlockId id = sink->id();
  WriterOptions opts;
  CFileWriter w(opts, STRING, false, sink.Pass());
  ASSERT_OK(w.Start());
  fs::ScopedWritableBlockCloser closer;
  ASSERT_OK(w.FinishAndReleaseBlock(&closer));
  if (FLAGS_cfile_do_on_finish == "flush") {
    ASSERT_EQ(1, closer.blocks().size());
    ASSERT_EQ(WritableBlock::FLUSHING, closer.blocks()[0]->state());
  } else if (FLAGS_cfile_do_on_finish == "close") {
    ASSERT_EQ(0, closer.blocks().size());
  } else if (FLAGS_cfile_do_on_finish == "nothing") {
    ASSERT_EQ(1, closer.blocks().size());
    ASSERT_EQ(WritableBlock::DIRTY, closer.blocks()[0]->state());
  } else {
    LOG(FATAL) << "Unknown value for cfile_do_on_finish: "
               << FLAGS_cfile_do_on_finish;
  }
  ASSERT_OK(closer.CloseBlocks());
  ASSERT_EQ(0, closer.blocks().size());
}

TEST_F(TestCFile, TestLazyInit) {
  // Create a small test file.
  BlockId block_id;
  {
    const int nrows = 1000;
    StringDataGenerator<false> generator("hello %04d");
    WriteTestFile(&generator, PREFIX_ENCODING, NO_COMPRESSION, nrows,
                  SMALL_BLOCKSIZE | WRITE_VALIDX, &block_id);
  }

  // Open it using a "counting" readable block.
  gscoped_ptr<ReadableBlock> block;
  ASSERT_OK(fs_manager_->OpenBlock(block_id, &block));
  size_t bytes_read = 0;
  gscoped_ptr<ReadableBlock> count_block(
      new CountingReadableBlock(block.Pass(), &bytes_read));

  // Lazily opening the cfile should not trigger any reads.
  gscoped_ptr<CFileReader> reader;
  ASSERT_OK(CFileReader::OpenNoInit(
      count_block.Pass(), ReaderOptions(), &reader));
  ASSERT_EQ(0, bytes_read);

  // But initializing it should (only the first time).
  ASSERT_OK(reader->Init());
  ASSERT_GT(bytes_read, 0);
  size_t bytes_read_after_init = bytes_read;
  ASSERT_OK(reader->Init());
  ASSERT_EQ(bytes_read_after_init, bytes_read);

  // And let's test non-lazy open for good measure; it should yield the
  // same number of bytes read.
  ASSERT_OK(fs_manager_->OpenBlock(block_id, &block));
  bytes_read = 0;
  count_block.reset(new CountingReadableBlock(block.Pass(), &bytes_read));
  ASSERT_OK(CFileReader::Open(count_block.Pass(), ReaderOptions(), &reader));
  ASSERT_EQ(bytes_read_after_init, bytes_read);
}

} // namespace cfile
} // namespace kudu
