#pragma once

#include "IReferenceCounted.h"
#include "irrTypes.h"
#include "EHardwareBufferFlags.h"

namespace scene
{

struct HWBuffer : public virtual IReferenceCounted {
	enum class Type
	{
		VERTEX,
		INDEX,
		WEIGHT,
	};
	/// Type of the buffer for faster type checks than dynamic_cast
	virtual Type getBufferType() const = 0;

	/// Size of one element in bytes
	virtual u32 getElementSize() const = 0;
	/// Number of elements in the buffer
	virtual u32 getCount() const = 0;
	/// Pointer to the buffer data
	virtual const void *getData() const = 0;

	/// Get the currently used ID for identification of changes. Should be used only by driver.
	u32 getChangedID() const { return ChangedID; }

	/// Marks the buffer as changed, so that hardware buffers are reloaded
	void setDirty();

	//! hardware mapping hint
	E_HARDWARE_MAPPING MappingHint = EHM_NEVER;
	//! link back to driver specific buffer info
	mutable void *Link = nullptr;

	/// If set, the driver frees the CPU-side copy of the data once it has
	/// been uploaded to a hardware buffer. Saves memory for static geometry
	/// that is only ever drawn. Afterwards the buffer is immutable: getCount()
	/// still works, but getData() returns nullptr.
	bool ReleaseDataAfterUpload = false;

	/// @return whether the CPU-side data has been released
	bool isDataReleased() const { return DataReleased; }

	/// Frees the CPU-side data but remembers the element count.
	/// Only to be called by the driver after a successful upload.
	virtual void releaseData() {}

protected:
	bool DataReleased = false;

private:

	u32 ChangedID = 1;
	static constexpr bool DEBUG = false;
};

} // end namespace scene
