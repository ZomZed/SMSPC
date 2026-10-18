#include <string.h>
#include <JSystem/JKernel/JKRArchive.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTAssert.hpp>
#include <ctype.h>

u32 JKRArchive::sCurrentDirID;

JKRArchive::JKRArchive()
{
	mIsMounted      = false;
	mMountDirection = MOUNT_DIRECTION_HEAD;
#ifdef SMS_PORT
	mFileData = nullptr;
#endif
}

JKRArchive::JKRArchive(intptr_t entryNumber, JKRArchive::EMountMode mountMode)
{
#ifdef SMS_PORT
	mFileData = nullptr;
#endif
	mIsMounted  = false;
	mMountMode  = mountMode;
	mMountCount = 1;
	_54         = 1;

	mHeap = JKRHeap::findFromRoot(this);
	if (mHeap == nullptr) {
		mHeap = JKRHeap::getCurrentHeap();
	}

	mEntryNum = entryNumber;
	if (sCurrentVolume == NULL) {
		sCurrentVolume = this;
		sCurrentDirID  = 0;
	}
}

JKRArchive::JKRArchive(const char* p1, JKRArchive::EMountMode mountMode)
{
#ifdef SMS_PORT
	mFileData = nullptr;
#endif
	JUT_ASSERT_F(false, "UNIMPLEMENTED");
}

#ifdef SMS_PORT
JKRArchive::~JKRArchive() { portFreeFileData(); }

// Called by every subclass right after mArcInfoBlock/mFileEntries are set up.
// Any previous table (remount of a fixed archive) is released first.
void JKRArchive::portAllocFileData()
{
	portFreeFileData();
	u32 count = mArcInfoBlock ? mArcInfoBlock->num_file_entries : 0;
	mFileData = (void**)::calloc(count != 0 ? count : 1, sizeof(void*));
	if (mFileData == nullptr) {
		OSPanic(__FILE__, __LINE__, "JKRArchive: no memory for %u file pointers", count);
	}
}

void JKRArchive::portFreeFileData()
{
	::free(mFileData);
	mFileData = nullptr;
}
#else
JKRArchive::~JKRArchive() { }
#endif

bool JKRArchive::isSameName(JKRArchive::CArcName& name, u32 nameOffset,
                            u16 nameHash) const
{
	u16 hash = name.getHash();
	if (hash != nameHash)
		return false;
	return strcmp(mStrTable + nameOffset, name.getString()) == 0;
}

JKRArchive::SDIDirEntry* JKRArchive::findResType(u32 type) const
{
	SDIDirEntry* node = mDirectories;
	u32 count         = 0;
	while (count < mArcInfoBlock->num_nodes) {
		if (node->mType == type) {
			return node;
		}

		node++;
		count++;
	}

	return nullptr;
}

#ifdef SMS_PORT
// A path through ".." from the root (e.g. TEnemyMario's "/../map/pad3/Setting.prm")
// follows the root's ".." entry, whose data offset is 0xFFFFFFFF. On the GameCube
// `mDirectories + 0xFFFFFFFF` wraps to mDirectories[-1], the last 16 bytes of the
// info block; on x64 it pointed 64 GB away (crash found by UBSan/ASan in Noki Bay).
// Reproduce the GameCube read: the info block's string-table length/offset and
// next-free-file-id were swapped as such (same values as GameCube reads), and the
// remaining bytes (sync flag + padding) are read big-endian as mNum / mFirstIdx.
JKRArchive::SDIDirEntry* JKRArchive::portDirEntry(u32 directoryId, SDIDirEntry* tmp) const
{
	if ((s32)directoryId >= 0)
		return mDirectories + directoryId;
	const u8* raw = (const u8*)(mDirectories + (s32)directoryId);
	memcpy(tmp, raw, sizeof(*tmp));
	if ((s32)directoryId == -1) {
		tmp->mNum      = (u16)(raw[0xA] << 8 | raw[0xB]);
		tmp->mFirstIdx = (u32)raw[0xC] << 24 | (u32)raw[0xD] << 16 | (u32)raw[0xE] << 8 | raw[0xF];
	}
	return tmp;
}
#endif

JKRArchive::SDIDirEntry* JKRArchive::findDirectory(const char* name,
                                                   u32 directoryId) const
{
	if (name == nullptr) {
#ifdef SMS_PORT
		return mDirectories + (s32)directoryId; // GameCube 32-bit wrap
#else
		return mDirectories + directoryId;
#endif
	}

	CArcName arcName(&name, '/');
#ifdef SMS_PORT
	SDIDirEntry portTmp;
	SDIDirEntry* dirEntry = portDirEntry(directoryId, &portTmp);
#else
	SDIDirEntry* dirEntry   = mDirectories + directoryId;
#endif
	SDIFileEntry* fileEntry = mFileEntries + dirEntry->mFirstIdx;

	for (int i = 0; i < dirEntry->mNum; i++) {
		if (isSameName(arcName, fileEntry->mFlagsAndNameOffset & 0xFFFFFF,
		               fileEntry->mHash)) {
			if (fileEntry->mFlagsAndNameOffset >> 24 & 0x02) {
				return findDirectory(name, fileEntry->mDataOffset);
			}
			break;
		}
		fileEntry++;
	}

	return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findTypeResource(u32 type, u32 id) const
{
	JUT_ASSERT_F(false, "UNIMPLEMENTED");
	return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findTypeResource(u32 type,
                                                       const char* name) const
{
	if (type) {
		CArcName arcName(name);
		SDIDirEntry* dirEntry = findResType(type);

		if (dirEntry) {
			SDIFileEntry* fileEntry = mFileEntries + dirEntry->mFirstIdx;
			for (int i = 0; i < dirEntry->mNum; fileEntry++, i++) {
				if (isSameName(arcName,
				               fileEntry->mFlagsAndNameOffset & 0xFFFFFF,
				               fileEntry->mHash)) {
					return fileEntry;
				}
			}
		}
	}

	return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findFsResource(const char* name,
                                                     u32 directoryId) const
{
	if (name) {
		CArcName arcName(&name, '/');
#ifdef SMS_PORT
		SDIDirEntry portTmp;
		SDIDirEntry* dirEntry = portDirEntry(directoryId, &portTmp);
#else
		SDIDirEntry* dirEntry   = mDirectories + directoryId;
#endif
		SDIFileEntry* fileEntry = mFileEntries + dirEntry->mFirstIdx;

		for (int i = 0; i < dirEntry->mNum; ++i) {
			if (isSameName(arcName, fileEntry->mFlagsAndNameOffset & 0xFFFFFF,
			               fileEntry->mHash)) {
				if (fileEntry->mFlagsAndNameOffset >> 24 & 0x02) {
					return findFsResource(name, fileEntry->mDataOffset);
				}

				if (name == nullptr) {
					return fileEntry;
				}

				return nullptr;
			}
			fileEntry++;
		}
	}

	return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findIdxResource(u32 fileIndex) const
{
	if (fileIndex < mArcInfoBlock->num_file_entries) {
		return mFileEntries + fileIndex;
	}

	return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findNameResource(const char* name) const
{
	SDIFileEntry* fileEntry = mFileEntries;

	CArcName arcName(name);
	for (int i = 0; i < mArcInfoBlock->num_file_entries; fileEntry++, i++) {
		if (isSameName(arcName, fileEntry->mFlagsAndNameOffset & 0xFFFFFF,
		               fileEntry->mHash)) {
			return fileEntry;
		}
	}

	return nullptr;
}

JKRArchive::SDIFileEntry*
JKRArchive::findPtrResource(const void* resource) const
{
	SDIFileEntry* fileEntry = mFileEntries;
	for (int i = 0; i < mArcInfoBlock->num_file_entries; fileEntry++, i++) {
		if (JKR_ARC_ENTRY_DATA(fileEntry) == resource) {
			return fileEntry;
		}
	}

	return nullptr;
}

JKRArchive::SDIFileEntry* JKRArchive::findIdResource(u16 id) const
{
	if (id != 0xFFFF) {
		SDIFileEntry* fileEntry;
		if (id < mArcInfoBlock->num_file_entries) {
			fileEntry = mFileEntries + id;
			if (fileEntry->mFileID == id
			    && fileEntry->mFlagsAndNameOffset >> 24 & 0x01) {
				return fileEntry;
			}
		}

		fileEntry = mFileEntries;
		for (int i = 0; i < mArcInfoBlock->num_file_entries; fileEntry++, i++) {
			if (fileEntry->mFileID == id
			    && fileEntry->mFlagsAndNameOffset >> 24 & 0x01) {
				return fileEntry;
			}
		}
	}

	return nullptr;
}

JKRArchive::CArcName::CArcName() { JUT_ASSERT_F(false, "UNIMPLEMENTED"); }

void JKRArchive::CArcName::store(const char* name)
{
	mHash      = 0;
	s32 length = 0;
	while (*name) {
		s32 ch = tolower(*name);
		mHash  = ch + mHash * 3;
		if (length < (s32)ARRAY_COUNT(mString) - 1) {
			mString[length++] = ch;
		}
		name++;
	}

	mLength         = (u16)length;
	mString[length] = 0;
}

const char* JKRArchive::CArcName::store(const char* name, char endChar)
{
	mHash      = 0;
	s32 length = 0;
	while (*name && *name != endChar) {
		s32 lch = tolower((int)*name);
		mHash   = lch + mHash * 3;
		if (length < (s32)ARRAY_COUNT(mString) - 1) {
			mString[length++] = lch;
		}
		name++;
	}

	mLength         = (u16)length;
	mString[length] = 0;

	if (*name == 0)
		return nullptr;
	return name + 1;
}
