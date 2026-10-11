import logging
import os
import random
import string
import unittest
import uuid
from typing import Union

from pyBIG import InDiskArchive, InMemoryArchive, base_archive
from pyBIG.refpack import compress, decompress, has_refpack_header

logging.basicConfig(level=logging.INFO)


TEST_FILE = "read_me_for_test.txt"
TEST_CONTENT = "john"
TEST_ENCODING = "latin-1"
TEST_ARCHIVE = "tests/test_data/empty_archive.big"


def string_generator(length: int) -> str:
    return "".join(random.choices(string.ascii_uppercase + string.digits, k=length))


class BaseTestCases:
    class BaseTest(unittest.TestCase):
        archive: Union[InMemoryArchive, InDiskArchive]

        def empty(self, header: str = "BIG4") -> base_archive.BaseArchive:
            raise NotImplementedError

        def open_from_path(self, path: str) -> base_archive.BaseArchive:
            raise NotImplementedError

        def tearDown(self):
            for file in [
                f"tests/test_data/output/{TEST_FILE}",
                "tests/test_data/output/test.big",
                TEST_ARCHIVE,
                "tests/test_data/test_big_type.big",
                "tests/test_data/test_offset.big",
            ]:
                try:
                    os.remove(file)
                except OSError:
                    pass

        def test_decode(self):
            contents = self.archive.read_file(TEST_FILE)

            with open(f"tests/test_data/{TEST_FILE}", "rb") as f:
                self.assertEqual(f.read(), contents)

            self.assertEqual(len(self.archive.entries), 1)

        def test_remove_file(self):
            self.archive.remove_file(TEST_FILE)
            self.archive.repack()

            self.assertEqual(self.archive.modified_entries, {})
            self.assertNotIn(TEST_FILE, self.archive.entries)

        def test_edit_file(self):
            self.archive.edit_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))
            self.archive.repack()

            contents = self.archive.read_file(TEST_FILE).decode(TEST_ENCODING)

            self.assertEqual(contents, TEST_CONTENT)

        def test_add_file(self):
            if self.archive.file_exists(TEST_FILE):
                self.archive.remove_file(TEST_FILE)
                self.archive.repack()

            self.archive.add_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))
            entry = self.archive.get_file_entry(TEST_FILE)
            self.assertEqual(entry.size, len(TEST_CONTENT.encode(TEST_ENCODING)))
            self.assertEqual(entry.position, -1)

            self.archive.repack()

            contents = self.archive.read_file(TEST_FILE).decode(TEST_ENCODING)

            self.assertEqual(contents, TEST_CONTENT)

        def test_extract_and_load(self):
            self.archive.extract("tests/test_data/output")
            self.assertTrue(os.path.exists(f"tests/test_data/output/{TEST_FILE}"))

            new_archive = InMemoryArchive.from_directory("tests/test_data/output")
            self.assertIn(TEST_FILE, new_archive.entries)

            new_archive.save("tests/test_data/output/test.big")
            self.assertTrue(os.path.exists("tests/test_data/output/test.big"))

        def test_utils(self):
            file_list = self.archive.file_list()
            self.archive.get_file_entry(file_list[0])

        def test_add_file_slash_in_name(self):
            with self.assertRaises(ValueError):
                self.archive.add_file("folder/" + TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))

        def test_validate_name(self):
            # editing an existing file with a valid name should still work. Note:
            # repack() is intentionally not called here so this test doesn't
            # permanently mutate on-disk fixtures shared across test cases.
            self.archive.edit_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))

            contents = self.archive.read_file(TEST_FILE).decode(TEST_ENCODING)
            self.assertEqual(contents, TEST_CONTENT)

            # but adding a file with a name that isn't latin-1 encodable must be rejected
            with self.assertRaises(ValueError):
                self.archive.add_file("日本語.txt", TEST_CONTENT.encode(TEST_ENCODING))

        def test_archive_memory_size(self):
            archive = InDiskArchive.empty(file_path=TEST_ARCHIVE)
            file_bytes = TEST_CONTENT.encode(TEST_ENCODING)
            archive.add_file(TEST_FILE, file_bytes)

            size = archive.archive_memory_size()

            self.assertEqual(size, len(file_bytes))

        def test_archive_bytes(self):
            data = self.archive.bytes()
            self.assertIsInstance(data, bytes)

        def test_offsets(self):
            archive: base_archive.BaseArchive = self.empty()
            SIZE = 25
            path = "tests/test_data/test_offset.big"
            titles = [str(uuid.uuid4()) for _ in range(SIZE)]
            values = [string_generator(50).encode("utf-8") for _ in range(SIZE)]

            for x in range(SIZE):
                archive.add_file(titles[x], values[x])

            archive.repack()

            for x in range(SIZE):
                contents = archive.read_file(titles[x])
                self.assertEqual(values[x], contents, "Offset incorrect after adding files")

            archive.save(path)
            archive: base_archive.BaseArchive = self.open_from_path(path)

            for x in range(SIZE):
                contents = archive.read_file(titles[x])
                self.assertEqual(values[x], contents, "Offset incorrect after saving archive")

            for _ in range(5):
                pop = random.choice(range(len(titles)))
                name = titles.pop(pop)
                print(name)
                values.pop(pop)
                archive.remove_file(name)

            archive.save(path)
            archive: base_archive.BaseArchive = self.open_from_path(path)

            for x in range(len(titles)):
                contents = archive.read_file(titles[x])
                self.assertEqual(
                    values[x], contents, "Offset incorrect after removing files and saving archive"
                )


class TestArchive(BaseTestCases.BaseTest):
    def setUp(self):
        with open("tests/test_data/test_big.big", "rb") as f:
            self.archive = InMemoryArchive(f.read())

    def empty(self, header: str = "BIG4") -> base_archive.BaseArchive:
        return InMemoryArchive.empty(header)

    def open_from_path(self, path: str) -> base_archive.BaseArchive:
        with open(path, "rb") as f:
            return InMemoryArchive(f.read())

    def test_empty_archive(self):
        archive = self.empty()
        archive.add_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))
        archive.repack()

        self.assertIn(TEST_FILE, archive.entries)

    def test_archive_type(self):
        path = "tests/test_data/test_big_type.big"

        for header in ["BIG4", "BIGF"]:
            archive = self.empty(header)
            archive.add_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))
            archive.save(path)

            with open(path, "rb") as f:
                archive = InMemoryArchive(f.read())

            self.assertEqual(archive.header, header)
            os.remove("tests/test_data/test_big_type.big")

    def test_invalid_header(self):
        with self.assertRaises(ValueError):
            InMemoryArchive()

        with self.assertRaises(ValueError):
            InMemoryArchive(b"garbage data here")

    def test_extract_path_traversal(self):
        archive = self.empty()
        archive.add_file("..\\evil.txt", TEST_CONTENT.encode(TEST_ENCODING))

        with self.assertRaises(ValueError):
            archive.extract("tests/test_data/output/traversal_test")

    def test_empty_archive_bytes(self):
        # regression test for the skip-needless-repack optimization: an empty
        # archive's internal buffer starts out as b"", so bytes() must still
        # force a repack instead of returning that empty buffer as-is.
        data = InMemoryArchive.empty().bytes()

        new_archive = InMemoryArchive(data)
        self.assertEqual(len(new_archive.entries), 0)


class TestLargeArchive(BaseTestCases.BaseTest):
    def setUp(self):
        self.archive = InDiskArchive("tests/test_data/test_big.big")

        if self.archive.file_exists(TEST_FILE):
            self.archive.remove_file(TEST_FILE)

        if not self.archive.file_exists(TEST_FILE):
            with open(f"tests/test_data/{TEST_FILE}", "rb") as f:
                self.archive.add_file(TEST_FILE, f.read())

        self.archive.repack()

    def empty(self, header: str = "BIG4"):
        return InDiskArchive.empty(header, file_path="tests/test_data/test_big_type.big")

    def open_from_path(self, path: str) -> base_archive.BaseArchive:
        return InDiskArchive(path)

    def test_empty_archive(self):
        archive = InDiskArchive.empty(file_path=TEST_ARCHIVE)
        archive.add_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))
        archive.repack()

        self.assertIn(TEST_FILE, archive.entries)

    def test_archive_type(self):
        path = "tests/test_data/test_big_type.big"

        for header in ["BIG4", "BIGF"]:
            archive = InDiskArchive.empty(header, file_path=path)
            archive.add_file(TEST_FILE, TEST_CONTENT.encode(TEST_ENCODING))
            archive.save()
            archive = InDiskArchive(path)
            self.assertEqual(archive.header, header)
            os.remove("tests/test_data/test_big_type.big")

    def test_save_new_path_updates_file_path(self):
        path_a = "tests/test_data/save_regression_a.big"
        path_b = "tests/test_data/save_regression_b.big"

        try:
            archive = InDiskArchive.empty(file_path=path_a)
            archive.add_file("first.txt", b"first content")
            archive.save()

            archive.add_file("second.txt", b"second content")
            archive.save(path_b)

            # both files must be readable from the same instance after saving
            # to a new path, since self.file_path needs to track the new location
            self.assertEqual(archive.read_file("first.txt"), b"first content")
            self.assertEqual(archive.read_file("second.txt"), b"second content")
        finally:
            for file in (path_a, path_b):
                try:
                    os.remove(file)
                except OSError:
                    pass


class TestRefPack(unittest.TestCase):
    def test_refpack_check_valid_data(self):
        data = b"Sample data for testing."
        compressed = compress(data)
        self.assertTrue(has_refpack_header(compressed))

    def test_refpack_check_invalid_magic(self):
        invalid_data = b"\x00\x00\x00\x00"
        self.assertFalse(has_refpack_header(invalid_data))

    def test_refpack_check_invalid_size(self):
        # Construct data with valid magic but invalid size (0)
        data = b"\x10\xfb\x00\x00"
        self.assertFalse(has_refpack_header(data))

    def test_compress_decompress_roundtrip(self):
        data = b"Example data to compress and decompress."
        compressed = compress(data)
        decompressed = decompress(compressed)
        self.assertEqual(decompressed, data)

    def test_decompress_size_mismatch_raises(self):
        data = b"Test data with mismatch"
        compressed = compress(data)
        corrupted = bytearray(compressed)
        corrupted[2:5] = b"\x00\x00\x00"  # corrupt expected size
        with self.assertRaises(ValueError):
            decompress(bytes(corrupted), ignore_mismatch=False)

    def test_decompress_size_mismatch_ignore(self):
        data = b"Test data with mismatch"
        compressed = compress(data)
        corrupted = bytearray(compressed)
        corrupted[2:5] = b"\x00\x00\x00"
        decompressed = decompress(bytes(corrupted), ignore_mismatch=True)
        self.assertTrue(decompressed)  # decompress returns bytes even if size mismatch ignored

    def test_empty_data(self):
        data = b""
        compressed = compress(data)
        decompressed = decompress(compressed)
        self.assertEqual(decompressed, data)

    def test_large_data(self):
        data = b"A" * 10_000
        compressed = compress(data)
        decompressed = decompress(compressed)
        self.assertEqual(decompressed, data)

    def test_periodic_data_overlapping_backreferences(self):
        # highly repetitive periodic data forces back-references whose
        # length exceeds their offset, i.e. overlapping copies
        for data in (b"abc" * 5000, b"ab" * 5000):
            compressed = compress(data)
            decompressed = decompress(compressed)
            self.assertEqual(decompressed, data)

    def test_pseudo_random_data_roundtrip(self):
        random.seed(42)
        data = bytes(random.getrandbits(8) for _ in range(50_000))
        compressed = compress(data)
        decompressed = decompress(compressed)
        self.assertEqual(decompressed, data)


if __name__ == "__main__":
    # python -m unittest
    unittest.main()
