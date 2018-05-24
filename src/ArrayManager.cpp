// ---------------------------------------------------------------------
// Copyright (c) 2016, Lawrence Livermore National Security, LLC. All
// rights reserved.
// 
// Produced at the Lawrence Livermore National Laboratory.
// 
// This file is part of CHAI.
// 
// LLNL-CODE-705877
// 
// For details, see https:://github.com/LLNL/CHAI
// Please also see the NOTICE and LICENSE files.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 
// - Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// 
// - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the
//   distribution.
// 
// - Neither the name of the LLNS/LLNL nor the names of its contributors
//   may be used to endorse or promote products derived from this
//   software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
// OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
// AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
// WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
// ---------------------------------------------------------------------
#include "chai/ArrayManager.hpp"

#include "chai/config.hpp"

#if defined(CHAI_ENABLE_CUDA)
#include "cuda_runtime_api.h"
#endif

#include "umpire/ResourceManager.hpp"

namespace chai {

ArrayManager* ArrayManager::s_resource_manager_instance = nullptr;
PointerRecord ArrayManager::s_null_record = PointerRecord();

ArrayManager* ArrayManager::getInstance() {
  if (!s_resource_manager_instance) {
    s_resource_manager_instance = new ArrayManager();
  }

  return s_resource_manager_instance;
}

ArrayManager::ArrayManager() :
  m_pointer_map(),
  m_resource_manager(umpire::ResourceManager::getInstance())
{
  m_pointer_map.clear();
  m_current_execution_space = NONE;
  m_default_allocation_space = CPU;

  m_allocators[CPU] = new umpire::Allocator(m_resource_manager.getAllocator("HOST"));
#if defined(CHAI_ENABLE_CUDA)
  m_allocators[GPU] = new umpire::Allocator(m_resource_manager.getAllocator("DEVICE"));
#endif
#if defined(CHAI_ENABLE_UM)
  m_allocators[UM] = new umpire::Allocator(m_resource_manager.getAllocator("UM"));
#endif
}

void ArrayManager::registerPointer(void* pointer, size_t size, ExecutionSpace space, bool owned) {
  CHAI_LOG("ArrayManager", "Registering " << pointer << " in space " << space);

  auto found_pointer_record = m_pointer_map.find(pointer);

  if (found_pointer_record != m_pointer_map.end()) {
  } else {
    m_pointer_map[pointer] = new PointerRecord();
  }

  auto & pointer_record = m_pointer_map[pointer];

  pointer_record->m_pointers[space] = pointer;
  pointer_record->m_size = size;
  pointer_record->m_last_space = space;

  for (int i = 0; i < NUM_EXECUTION_SPACES; i++) {
    pointer_record->m_owned[i] = true;
  }
  pointer_record->m_owned[space] = owned;
  pointer_record->m_user_callback = [](Action, ExecutionSpace, size_t){};
}

void ArrayManager::registerPointer(void* pointer, PointerRecord* record, ExecutionSpace space) 
{
  CHAI_LOG("ArrayManager", "Registering " << pointer << " in space " << space);

  record->m_pointers[space] = pointer;
  m_pointer_map[pointer] = record;
}

void ArrayManager::deregisterPointer(PointerRecord* record)
{
  for (int i = 0; i < NUM_EXECUTION_SPACES; i++) {
    if (record->m_pointers[i])
      m_pointer_map.erase(record->m_pointers[i]);
  }

  delete record;
}

void ArrayManager::setExecutionSpace(ExecutionSpace space) {
  CHAI_LOG("ArrayManager", "Setting execution space to " << space);

  m_current_execution_space = space;
}

void* ArrayManager::move(void* pointer, ExecutionSpace space) {

  // Check for default arg (NONE)
  if (space == NONE)
  {
    space = m_current_execution_space;
  }

  if (space == NONE) {
    return pointer;
  }

  auto pointer_record = getPointerRecord(pointer);
  move(pointer_record, space);

  return pointer_record->m_pointers[space];
}

ExecutionSpace ArrayManager::getExecutionSpace() {
  return m_current_execution_space;
}

void ArrayManager::registerTouch(void* pointer) {
  CHAI_LOG("ArrayManager", pointer << " touched in space " << m_current_execution_space);
  
  if (m_current_execution_space == NONE)
    return;

  registerTouch(pointer, m_current_execution_space);
}

void ArrayManager::registerTouch(void* pointer, ExecutionSpace space) {
  auto pointer_record = getPointerRecord(pointer);
  pointer_record->m_touched[space] = true;
  pointer_record->m_last_space = space;
}


void ArrayManager::resetTouch(void* pointer)
{
  auto record = getPointerRecord(pointer);
  resetTouch(record);
}


void ArrayManager::resetTouch(PointerRecord* pointer_record) {
  for (int space = CPU; space < NUM_EXECUTION_SPACES; ++space) {
    pointer_record->m_touched[space] = false;
  }
}

void ArrayManager::move(PointerRecord* record, ExecutionSpace space) 
{
  if ( space == NONE ) {
    return;
  }

#if defined(CHAI_ENABLE_UM)
  if (record->m_last_space == UM) {
    return;
  }
#endif

  if (space == record->m_last_space) {
    return;
  }


  void* src_pointer = record->m_pointers[record->m_last_space];
  void* dst_pointer = record->m_pointers[space];

  if (!dst_pointer) {
    allocate(record, space);
    dst_pointer = record->m_pointers[space];
  }

  if (!record->m_touched[record->m_last_space]) {
    return;
  } else {
    record->m_user_callback(ACTION_MOVE, space, record->m_size);
    m_resource_manager.copy(dst_pointer, src_pointer);
  }

  resetTouch(record);
}

} // end of namespace chai
