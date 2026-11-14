#include <atomic>
#include "ItemUID.h"

static std::atomic<uint64_t> sNextItemUID = 1;

uint64_t NewItemUID()
{
	return sNextItemUID.fetch_add(1);
}

void SetLastItemUID(const uint64_t lastItemUID)
{
	sNextItemUID = lastItemUID + 1;
}
