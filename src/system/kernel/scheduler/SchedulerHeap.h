/*
 * Copyright 2013 Haiku, Inc. All rights reserved.
 * Copyright 2024 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Paweł Dziepak, pdziepak@quarnos.org
 *		Jules (OpenDevin)
 */
#ifndef KERNEL_SCHEDULER_HEAP_H
#define KERNEL_SCHEDULER_HEAP_H

#include <debug.h>
#include <SupportDefs.h>

// Forward declaration for friend class if SchedulerHeap is namespaced
namespace Scheduler {
template<typename Element, typename Compare, typename GetLink> class SchedulerHeap;
}

// Note: The Key template parameter is kept for structural compatibility with the original
// Heap.h, but for EEVDF's SchedulerHeap, the Compare policy directly uses ElementType
// (ThreadData*) to derive comparison values (VirtualDeadline). HeapLink::fKey might
// become vestigial or store ElementType itself.
template<typename ElementType, typename KeyType = ElementType>
struct SchedulerHeapLink {
						SchedulerHeapLink();

			int			fIndex;
			KeyType		fKey; // May store ElementType or be unused if Compare derives key
};

// Using ElementType directly as KeyType for EEVDF case
template<typename ElementType>
class SchedulerHeapLinkImpl {
private:
	typedef SchedulerHeapLink<ElementType, ElementType> Link;

public:
	inline	Link*		GetSchedulerHeapLink();

private:
			Link		fSchedulerHeapLink;
};


// Compare policy for SchedulerHeap (used by EevdfRunQueue)
// This will be EevdfDeadlineCompare, which compares ElementType (ThreadData*)
// by their VirtualDeadline(). It does not use the KeyType stored in SchedulerHeapLink.
// Example structure (actual EevdfDeadlineCompare is defined elsewhere):
/*
template<typename ElementType> // KeyType is not used by this Compare
class SchedulerElementCompare {
public:
	inline	bool		operator()(ElementType a, ElementType b) const;
		// In EEVDF: return a->VirtualDeadline() < b->VirtualDeadline();
};
*/

// GetLink policy for SchedulerHeap
template<typename ElementType>
class SchedulerStandardGetLink {
private:
	typedef SchedulerHeapLink<ElementType, ElementType> Link;

public:
	inline	Link*		operator()(ElementType element) const;
};


#define SCHEDULER_HEAP_TEMPLATE_LIST	\
	template<typename ElementType, typename Compare, typename GetLink>
#define SCHEDULER_HEAP_CLASS_NAME	SchedulerHeap<ElementType, Compare, GetLink>

// The KeyType parameter is removed from SchedulerHeap class template,
// as Compare policy will work directly on ElementType.
// Compare policy (e.g., EevdfDeadlineCompare) must have:
//   bool IsKeyLess(ElementType, ElementType)
// GetLink policy (e.g., EevdfGetLink) must have:
//   SchedulerHeapLink<ElementType, ElementType>* operator()(ElementType)
template<typename ElementType,
	typename Compare,	// Example: EevdfDeadlineCompare
	typename GetLink>	// Example: EevdfGetLink
class SchedulerHeap {
public:
						SchedulerHeap();
						SchedulerHeap(int initialSize);
						// Constructor for EEVDF: Compare and GetLink are policy objects, not static
						SchedulerHeap(Compare comparePolicy, GetLink getLinkPolicy, int initialSize = 0);
						~SchedulerHeap();

	inline	ElementType	PeekRoot(int32 index = 0) const;

	// GetKey becomes less relevant if Compare policy derives key from ElementType
	// static	const KeyType&	GetKey(ElementType element);

	// ModifyKey is replaced by Update
	// inline	void		ModifyKey(ElementType element, KeyType newKey);
	inline	void		Update(ElementType element);


	inline	void		RemoveRoot();
	// Insert now takes only the element, key is derived or stored if needed by GetLink/Compare
	inline	status_t	Insert(ElementType element);
	inline  bool		IsEmpty() const { return fLastElement == 0; }


	inline	int32		Count() const	{ return fLastElement; }

	// Specific Remove for EEVDF which was in original EevdfRunQueue.cpp
	// This needs to be here as Heap.h didn't have a non-root Remove(element).
	// This is a complex operation to add to a standard heap.
	// For now, EevdfRunQueue::Remove will use this, and Update will use sift.
	// This is the O(log N) remove assuming index is known.
			void		Remove(ElementType element);


private:
			status_t	_GrowHeap(int minimalSize = 0);

			// Pass ElementType directly for comparison, as fKey in link might be unused
			void		_MoveUp(int32 index);
			void		_MoveDown(int32 index);


			ElementType*	fElements;
			int				fLastElement;
			int				fSize;

			Compare			fCompare;	// Store policy objects
			GetLink			fGetLink;
};


#if KDEBUG
template<typename ElementType, typename KeyType>
SchedulerHeapLink<ElementType, KeyType>::SchedulerHeapLink()
	:
	fIndex(-1)
{
	// fKey is default constructed
}
#else
template<typename ElementType, typename KeyType>
SchedulerHeapLink<ElementType, KeyType>::SchedulerHeapLink()
{
	// fIndex is uninitialized, fKey is default constructed
}
#endif


template<typename ElementType>
SchedulerHeapLink<ElementType, ElementType>*
SchedulerHeapLinkImpl<ElementType>::GetSchedulerHeapLink()
{
	return &fSchedulerHeapLink;
}


template<typename ElementType>
SchedulerHeapLink<ElementType, ElementType>*
SchedulerStandardGetLink<ElementType>::operator()(ElementType element) const
{
	// Assumes ElementType has a method GetSchedulerHeapLink()
	// This matches how EevdfGetLink will work with ThreadData.
	return element->GetSchedulerHeapLink();
}


SCHEDULER_HEAP_TEMPLATE_LIST
SCHEDULER_HEAP_CLASS_NAME::SchedulerHeap()
	:
	fElements(NULL),
	fLastElement(0),
	fSize(0),
	fCompare(),       // Default construct policies
	fGetLink()
{
}

SCHEDULER_HEAP_TEMPLATE_LIST
SCHEDULER_HEAP_CLASS_NAME::SchedulerHeap(Compare comparePolicy, GetLink getLinkPolicy, int initialSize)
	:
	fElements(NULL),
	fLastElement(0),
	fSize(0),
	fCompare(comparePolicy),
	fGetLink(getLinkPolicy)
{
	if (initialSize > 0)
		_GrowHeap(initialSize);
}


SCHEDULER_HEAP_TEMPLATE_LIST
SCHEDULER_HEAP_CLASS_NAME::SchedulerHeap(int initialSize)
	:
	fElements(NULL),
	fLastElement(0),
	fSize(0),
	fCompare(),       // Default construct policies
	fGetLink()
{
	_GrowHeap(initialSize);
}


SCHEDULER_HEAP_TEMPLATE_LIST
SCHEDULER_HEAP_CLASS_NAME::~SchedulerHeap()
{
	free(fElements);
}


SCHEDULER_HEAP_TEMPLATE_LIST
ElementType
SCHEDULER_HEAP_CLASS_NAME::PeekRoot(int32 index) const
{
	if (index < fLastElement)
		return fElements[index];
	return NULL;	// Assuming ElementType is a pointer type for NULL
}


SCHEDULER_HEAP_TEMPLATE_LIST
void
SCHEDULER_HEAP_CLASS_NAME::Update(ElementType element)
{
	SchedulerHeapLink<ElementType, ElementType>* link = fGetLink(element);
	ASSERT(link->fIndex >= 0 && link->fIndex < fLastElement);
	ASSERT(fElements[link->fIndex] == element);

	// The element's key (e.g., VirtualDeadline) is assumed to have been updated
	// by the caller *before* calling this Update method.
	// We try to move it up, then down. One of these will place it correctly.
	// If it's already in the right spot relative to parent and children after
	// the key change, neither will move it far.
	_MoveUp(link->fIndex);
	// It's possible _MoveUp changed link->fIndex, so use that for _MoveDown
	_MoveDown(link->fIndex);
}


SCHEDULER_HEAP_TEMPLATE_LIST
void
SCHEDULER_HEAP_CLASS_NAME::RemoveRoot()
{
	ASSERT(fLastElement > 0);
	SchedulerHeapLink<ElementType, ElementType>* link = fGetLink(fElements[0]);

#if KDEBUG
	ASSERT(link->fIndex == 0);
	link->fIndex = -1;
#endif

	fLastElement--;
	if (fLastElement > 0) {
		ElementType lastElement = fElements[fLastElement];
		fElements[0] = lastElement;
		fGetLink(lastElement)->fIndex = 0;
		_MoveDown(0);
	}
}

SCHEDULER_HEAP_TEMPLATE_LIST
void
SCHEDULER_HEAP_CLASS_NAME::Remove(ElementType element)
{
	SchedulerHeapLink<ElementType, ElementType>* link = fGetLink(element);
	ASSERT(link->fIndex >= 0 && link->fIndex < fLastElement);
	ASSERT(fElements[link->fIndex] == element);

	int32 indexToRemove = link->fIndex;

#if KDEBUG
	link->fIndex = -1; // Mark as removed
#endif

	fLastElement--;
	if (indexToRemove == fLastElement) {
		// It was the last element, nothing more to do
		return;
	}

	// Replace the removed element with the last element from the heap
	ElementType lastElement = fElements[fLastElement];
	fElements[indexToRemove] = lastElement;
	fGetLink(lastElement)->fIndex = indexToRemove;

	// Restore heap property: try to move the swapped element up, then down.
	// One of these will ensure correctness.
	// The comparison should be against its old key if we had it, but since Compare
	// uses the element's current state, we just try both.
	// Or, more precisely, compare with parent to decide if _MoveUp is needed,
	// otherwise _MoveDown.
	if (indexToRemove > 0 && fCompare.IsKeyLess(fElements[indexToRemove], fElements[(indexToRemove - 1) / 2])) {
		_MoveUp(indexToRemove);
	} else {
		_MoveDown(indexToRemove);
	}
}


SCHEDULER_HEAP_TEMPLATE_LIST
status_t
SCHEDULER_HEAP_CLASS_NAME::Insert(ElementType element)
{
	if (fLastElement == fSize) {
		status_t result = _GrowHeap();
		if (result != B_OK)
			return result;
	}
	ASSERT(fLastElement < fSize);

	SchedulerHeapLink<ElementType, ElementType>* link = fGetLink(element);
	ASSERT(link->fIndex == -1 && "Element already in heap?");

	fElements[fLastElement] = element;
	link->fIndex = fLastElement;
	// link->fKey = element; // Store element as key if KeyType is ElementType
						  // This line is removed as fKey is not actively used by EevdfCompare policy
	fLastElement++;
	_MoveUp(link->fIndex);

	return B_OK;
}


SCHEDULER_HEAP_TEMPLATE_LIST
status_t
SCHEDULER_HEAP_CLASS_NAME::_GrowHeap(int minimalSize)
{
	int newSize = max_c(max_c(fSize * 2, 4), minimalSize);

	size_t arraySize = newSize * sizeof(ElementType);
	ElementType* newBuffer
		= reinterpret_cast<ElementType*>(realloc(fElements, arraySize));
	if (newBuffer == NULL)
		return B_NO_MEMORY;

	fElements = newBuffer;
	fSize = newSize;

	return B_OK;
}


SCHEDULER_HEAP_TEMPLATE_LIST
void
SCHEDULER_HEAP_CLASS_NAME::_MoveUp(int32 index)
{
	ElementType currentElement = fElements[index];
	SchedulerHeapLink<ElementType, ElementType>* currentLink = fGetLink(currentElement);

	while (index > 0) {
		int32 parentIndex = (index - 1) / 2;
		ElementType parentElement = fElements[parentIndex];

		if (fCompare.IsKeyLess(currentElement, parentElement)) {
			// Swap elements
			fElements[index] = parentElement;
			fGetLink(parentElement)->fIndex = index;

			fElements[parentIndex] = currentElement;
			currentLink->fIndex = parentIndex;

			index = parentIndex;
		} else
			break;
	}
}


SCHEDULER_HEAP_TEMPLATE_LIST
void
SCHEDULER_HEAP_CLASS_NAME::_MoveDown(int32 index)
{
	ElementType currentElement = fElements[index];
	SchedulerHeapLink<ElementType, ElementType>* currentLink = fGetLink(currentElement);
	int32 smallestChildIndex;

	while (true) {
		int32 leftChildIndex = 2 * index + 1;
		int32 rightChildIndex = 2 * index + 2;

		if (leftChildIndex < fLastElement) {
			smallestChildIndex = leftChildIndex;
			if (rightChildIndex < fLastElement
				&& fCompare.IsKeyLess(fElements[rightChildIndex], fElements[leftChildIndex])) {
				smallestChildIndex = rightChildIndex;
			}

			if (fCompare.IsKeyLess(fElements[smallestChildIndex], currentElement)) {
				// Swap with smallest child
				ElementType childElement = fElements[smallestChildIndex];
				fElements[index] = childElement;
				fGetLink(childElement)->fIndex = index;

				fElements[smallestChildIndex] = currentElement;
				currentLink->fIndex = smallestChildIndex;

				index = smallestChildIndex;
			} else
				break; // Heap property satisfied
		} else
			break; // No children
	}
}

#endif	// KERNEL_SCHEDULER_HEAP_H
