/*
 *  Copyright (c) 2026, MafiaHub
 *
 *  This source code is licensed under the MIT-style license found in the
 *  license.txt file in the root directory of this source tree.
 */

/// \file instance_ptr.h
/// \brief Owning pointer for types created by GetInstance() and freed by DestroyInstance().
///
/// MafiaNet factory types (RakPeerInterface, plugins, ...) must be freed by the module that
/// allocated them, so a plain std::unique_ptr (std::default_delete) is wrong for them.
/// \ref MafiaNet::InstancePtr routes destruction through T::DestroyInstance instead.

#pragma once

#include <memory> // std::unique_ptr

namespace MafiaNet
{

/// \brief Deleter that calls T::DestroyInstance(p) for a non-null pointer.
template <class T>
struct InstanceDeleter
{
	void operator()(T *p) const noexcept
	{
		if (p)
			T::DestroyInstance(p);
	}
};

/// \brief std::unique_ptr that frees through T::DestroyInstance.
template <class T>
using InstancePtr = std::unique_ptr<T, InstanceDeleter<T>>;

/// \brief Create a T through T::GetInstance() and wrap it in an \ref InstancePtr.
template <class T>
InstancePtr<T> MakeInstance()
{
	return InstancePtr<T>(T::GetInstance());
}

} // namespace MafiaNet
