/**
 * Copyright (c) 2023 Peking University and Peking University
 * Changsha Institute for Computing and Digital Economy
 *
 * CraneSched is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of
 * the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS,
 * WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "crane/PublicHeader.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <ranges>

AllocatableResource& AllocatableResource::operator+=(
    const AllocatableResource& rhs) {
  cpu_count += rhs.cpu_count;
  memory_bytes += rhs.memory_bytes;
  memory_sw_bytes += rhs.memory_sw_bytes;
  return *this;
}

AllocatableResource& AllocatableResource::operator-=(
    const AllocatableResource& rhs) {
  cpu_count -= rhs.cpu_count;
  memory_bytes -= rhs.memory_bytes;
  memory_sw_bytes -= rhs.memory_sw_bytes;
  return *this;
}

bool operator<=(const AllocatableResource& lhs,
                const AllocatableResource& rhs) {
  if (lhs.cpu_count <= rhs.cpu_count && lhs.memory_bytes <= rhs.memory_bytes &&
      lhs.memory_sw_bytes <= rhs.memory_sw_bytes)
    return true;

  return false;
}

bool operator<(const AllocatableResource& lhs, const AllocatableResource& rhs) {
  if (lhs.cpu_count < rhs.cpu_count && lhs.memory_bytes < rhs.memory_bytes &&
      lhs.memory_sw_bytes < rhs.memory_sw_bytes)
    return true;

  return false;
}

bool operator==(const AllocatableResource& lhs,
                const AllocatableResource& rhs) {
  if (lhs.cpu_count == rhs.cpu_count && lhs.memory_bytes == rhs.memory_bytes &&
      lhs.memory_sw_bytes == rhs.memory_sw_bytes)
    return true;

  return false;
}

bool operator==(const Device& lhs, const Device& rhs) {
  return lhs.path == rhs.path;
}

bool operator<=(const DedicatedResource& lhs, const DedicatedResource& rhs) {
  for (const auto& [lhs_node_id, lhs_gres] : lhs.craned_id_gres_map) {
    for (const auto& [lhs_name, lhs_type_slots_map] :
         lhs_gres.name_type_slots_map) {
      if (rhs.contains(lhs_node_id) && rhs.at(lhs_node_id).contains(lhs_name)) {
        const auto& rhs_slots = rhs.craned_id_gres_map.at(lhs_node_id)
                                    .name_type_slots_map.at(lhs_name);
        if (!std::ranges::includes(rhs_slots, lhs_type_slots_map)) return false;
      } else {
        if (!lhs_type_slots_map.empty()) return false;
      }
    }
  }
  return true;
}

bool operator<(const DedicatedResource& lhs, const DedicatedResource& rhs) {
  bool all_element_equ_or_empty = true;
  for (const auto& [rhs_craned_id, rhs_gres] : rhs.craned_id_gres_map) {
    if (lhs.craned_id_gres_map.contains(rhs_craned_id)) {
      const auto& lhs_name_slots_map =
          lhs.craned_id_gres_map.at(rhs_craned_id).name_type_slots_map;
      const auto& rhs_name_slots_map = rhs_gres.name_type_slots_map;

      for (const auto& [rhs_name, rhs_slots] : rhs_name_slots_map) {
        if (!lhs_name_slots_map.contains(rhs_name)) {
          if (all_element_equ_or_empty && !rhs_slots.empty())
            all_element_equ_or_empty = false;
        } else {
          const auto& lhs_slots = lhs_name_slots_map.at(rhs_name);

          if (lhs_slots.size() > rhs_slots.size()) return false;
          if (all_element_equ_or_empty && rhs_slots != lhs_slots)
            all_element_equ_or_empty = false;
          if (!std::ranges::includes(rhs_slots, lhs_slots)) return false;
        }
      }

    } else {
      if (all_element_equ_or_empty && !rhs_gres.empty())
        all_element_equ_or_empty = false;
    }
  }
  if (all_element_equ_or_empty) return false;
  return true;
}

bool operator==(const DedicatedResource& lhs, const DedicatedResource& rhs) {
  std::unordered_set<std::string> keys;
  std::ranges::for_each(lhs.craned_id_gres_map,
                        [&keys](const auto& kv) { keys.emplace(kv.first); });
  std::ranges::for_each(rhs.craned_id_gres_map,
                        [&keys](const auto& kv) { keys.emplace(kv.first); });
  for (const auto& key : keys) {
    bool lhs_contains = lhs.craned_id_gres_map.contains(key);
    bool rhs_contains = rhs.craned_id_gres_map.contains(key);
    if (lhs_contains && rhs_contains) {
      const auto& lhs_name_slot_map =
          lhs.craned_id_gres_map.at(key).name_type_slots_map;
      const auto& rhs_name_slot_map =
          rhs.craned_id_gres_map.at(key).name_type_slots_map;
      std::unordered_set<std::string> inner_keys;
      std::ranges::for_each(lhs_name_slot_map, [&inner_keys](const auto& kv) {
        inner_keys.emplace(kv.first);
      });
      std::ranges::for_each(rhs_name_slot_map, [&inner_keys](const auto& kv) {
        inner_keys.emplace(kv.first);
      });
      for (const auto& inner_key : inner_keys) {
        bool inner_lhs_contains = lhs_name_slot_map.contains(key);
        bool inner_rhs_contains = rhs_name_slot_map.contains(key);

        if (inner_lhs_contains && inner_rhs_contains) {
          if (lhs_name_slot_map.at(inner_key) !=
              rhs_name_slot_map.at(inner_key))
            return false;
        } else if (!inner_lhs_contains && !inner_rhs_contains) {
          continue;
        } else {
          if (inner_lhs_contains) {
            if (!lhs_name_slot_map.at(inner_key).empty()) return false;
          } else {
            if (!rhs_name_slot_map.at(inner_key).empty()) return false;
          }
        }
      }

    } else if (!lhs_contains && !rhs_contains) {
      continue;
    } else {
      if (lhs_contains) {
        if (!lhs.craned_id_gres_map.at(key).empty()) return false;
      } else {
        // rhs_contains
        if (!rhs.craned_id_gres_map.at(key).empty()) return false;
      }
    }
  }
  return true;
}

bool operator<=(const DedicatedResourceInNode& lhs,
                const DedicatedResourceInNode& rhs) {
  for (const auto& [lhs_name, lhs_type_slots_map] : lhs.name_type_slots_map) {
    if (rhs.contains(lhs_name)) {
      const auto& rhs_slots = rhs.name_type_slots_map.at(lhs_name);
      if (!std::ranges::includes(rhs_slots, lhs_slots)) return false;
    } else {
      if (!std::ranges::for_each(lhs_type_slots_map, [](const auto& kv) {}))
        return false;
    }
  }
  return true;
}

AllocatableResource::AllocatableResource(
    const crane::grpc::AllocatableResource& value) {
  cpu_count = cpu_t{value.cpu_core_limit()};
  memory_bytes = value.memory_limit_bytes();
  memory_sw_bytes = value.memory_sw_limit_bytes();
}

AllocatableResource& AllocatableResource::operator=(
    const crane::grpc::AllocatableResource& value) {
  cpu_count = cpu_t{value.cpu_core_limit()};
  memory_bytes = value.memory_limit_bytes();
  memory_sw_bytes = value.memory_sw_limit_bytes();
  return *this;
}

Resources& Resources::operator+=(const Resources& rhs) {
  allocatable_resource += rhs.allocatable_resource;
  dedicated_resource += rhs.dedicated_resource;
  return *this;
}

Resources& Resources::operator-=(const Resources& rhs) {
  allocatable_resource -= rhs.allocatable_resource;
  dedicated_resource -= rhs.dedicated_resource;
  return *this;
}

Resources& Resources::operator+=(const AllocatableResource& rhs) {
  allocatable_resource += rhs;
  return *this;
}

Resources& Resources::operator-=(const AllocatableResource& rhs) {
  allocatable_resource -= rhs;
  return *this;
}
Resources Resources::operator+(const DedicatedResource& rhs) const {
  Resources result(*this);
  result.dedicated_resource += rhs;
  return result;
}

bool operator<=(const Resources& lhs, const Resources& rhs) {
  return lhs.allocatable_resource <= rhs.allocatable_resource &&
         lhs.dedicated_resource <= rhs.dedicated_resource;
}

bool operator<(const Resources& lhs, const Resources& rhs) {
  return lhs.allocatable_resource < rhs.allocatable_resource &&
         lhs.dedicated_resource < rhs.dedicated_resource;
}

bool operator==(const Resources& lhs, const Resources& rhs) {
  return lhs.allocatable_resource == rhs.allocatable_resource &&
         lhs.dedicated_resource == rhs.dedicated_resource;
}

DedicatedResource& DedicatedResource::operator+=(const DedicatedResource& rhs) {
  for (const auto& [rhs_node_id, rhs_name_slots_map] : rhs.craned_id_gres_map) {
    this->craned_id_gres_map[rhs_node_id] += rhs_name_slots_map;
  }

  return *this;
}

DedicatedResource& DedicatedResource::operator-=(const DedicatedResource& rhs) {
  for (const auto& [rhs_node_id, rhs_name_slots_map] : rhs.craned_id_gres_map) {
    if (!this->craned_id_gres_map.contains(rhs_node_id)) continue;
    this->craned_id_gres_map[rhs_node_id] -= rhs_name_slots_map;
  }

  return *this;
}
bool DedicatedResource::contains(const CranedId& craned_id) const {
  return craned_id_gres_map.contains(craned_id);
}

bool DedicatedResource::Empty() const {
  if (craned_id_gres_map.empty()) return true;
  return std::ranges::all_of(craned_id_gres_map,
                             [](const auto& kv) { return kv.second.empty(); });
}

DedicatedResource::DedicatedResource(
    const crane::grpc::DedicatedResource& rhs) {
  for (const auto& [craned_id, gres] : rhs.each_node_gres()) {
    auto& this_craned_gres_map =
        this->craned_id_gres_map[craned_id].name_type_slots_map;
    for (const auto& [name, type_slots_map] : gres.name_type_map()) {
      for (const auto& [type, slots] : type_slots_map.type_slots_map())
        this_craned_gres_map[name][type].insert(slots.slots().begin(),
                                                slots.slots().end());
    }
  }
}

DedicatedResource::operator crane::grpc::DedicatedResource() const {
  crane::grpc::DedicatedResource val{};
  for (const auto& [craned_id, gres] : craned_id_gres_map) {
    for (const auto& [name, type_slots_map] : gres.name_type_slots_map) {
      {
        for (const auto& [type, slots] : type_slots_map) {
          (*(*(*val.mutable_each_node_gres())[craned_id]
                  .mutable_name_type_map())[name]
                .mutable_type_slots_map())[type]
              .mutable_slots()
              ->Assign(slots.begin(), slots.end());
        }
      }
    }
  }
  return val;
}

DedicatedResourceInNode& DedicatedResource::operator[](
    const std::string& craned_id) {
  return this->craned_id_gres_map[craned_id];
}

const DedicatedResourceInNode& DedicatedResource::at(
    const std::string& craned_id) const {
  return this->craned_id_gres_map.at(craned_id);
}

DedicatedResourceInNode& DedicatedResource::at(const std::string& craned_id) {
  return this->craned_id_gres_map.at(craned_id);
}

std::optional<std::tuple<unsigned int, unsigned int, char>>
GetDeviceFileMajorMinorOpType(const std::string& path) {
  struct stat device_file_info {};
  if (stat(path.c_str(), &device_file_info) == 0) {
    char op_type = 'a';
    if (S_ISBLK(device_file_info.st_mode)) {
      op_type = 'b';
    } else if (S_ISCHR(device_file_info.st_mode)) {
      op_type = 'c';
    }
    return std::make_tuple(major(device_file_info.st_rdev),
                           minor(device_file_info.st_rdev), op_type);
  } else {
    return std::nullopt;
  }
}

bool Device::Init() {
  const auto& device_major_minor_optype_option =
      GetDeviceFileMajorMinorOpType(path);
  if (device_major_minor_optype_option.has_value()) {
    const auto& device_major_minor_optype =
        device_major_minor_optype_option.value();

    this->major = std::get<0>(device_major_minor_optype);
    this->minor = std::get<1>(device_major_minor_optype);
    this->op_type = std::get<2>(device_major_minor_optype);
  } else {
    return false;
  }
  return true;
}

bool Device::Init(const std::string& device_name,
                  const std::string& device_type,
                  const std::string& device_path) {
  this->type = device_type;
  this->name = device_name;
  const auto& device_major_minor_optype_option =
      GetDeviceFileMajorMinorOpType(device_path);
  if (device_major_minor_optype_option.has_value()) {
    const auto& device_major_minor_optype =
        device_major_minor_optype_option.value();

    this->major = std::get<0>(device_major_minor_optype);
    this->minor = std::get<1>(device_major_minor_optype);
    this->op_type = std::get<2>(device_major_minor_optype);
    this->path = device_path;
  } else {
    return false;
  }
  return true;
}
Device::Device(const std::string& device_name, const std::string& device_type,
               const std::string& device_path)
    : name(device_name), type(device_type), path(device_path){};

bool DedicatedResourceInNode::empty() const {
  if (name_type_slots_map.empty()) return true;
  return std::ranges::all_of(name_type_slots_map, [](const auto& kv) {
    return kv.second.empty() ||
           std::ranges::all_of(kv.second, [](const auto& inner_kv) {
             return inner_kv.second.empty();
           });
  });
}

bool DedicatedResourceInNode::empty(const std::string& device_name) const {
  if (name_type_slots_map.empty() || !name_type_slots_map.contains(device_name))
    return true;
  return std::ranges::all_of(name_type_slots_map.at(device_name),
                             [](const auto& kv) { return kv.second.empty(); });
}
bool DedicatedResourceInNode::empty(const std::string& device_name,
                                    const std::string& device_type) const {
  if (name_type_slots_map.empty() ||
      !name_type_slots_map.contains(device_name) ||
      !name_type_slots_map.at(device_name).contains(device_type))
    return true;
  return name_type_slots_map.at(device_name).at(device_type).empty();
}

DedicatedResourceInNode& DedicatedResourceInNode::operator+=(
    const DedicatedResourceInNode& rhs) {
  for (const auto& [rhs_name, rhs_type_slots_map] : rhs.name_type_slots_map) {
    for (const auto& [rhs_type, rhs_slots] : rhs_type_slots_map)
      this->name_type_slots_map[rhs_name][rhs_type].insert(rhs_slots.begin(),
                                                           rhs_slots.end());
  }
  return *this;
}

DedicatedResourceInNode& DedicatedResourceInNode::operator-=(
    const DedicatedResourceInNode& rhs) {
  for (const auto& [rhs_name, rhs_type_slots_map] : rhs.name_type_slots_map) {
    auto& this_type_slots_map = this->name_type_slots_map.at(rhs_name);
    for (const auto& [rhs_type, rhs_slots] : rhs_type_slots_map) {
      std::set<SlotId> temp;
      std::ranges::set_difference(this_type_slots_map.at(rhs_type), rhs_slots,
                                  std::inserter(temp, temp.begin()));
      this_type_slots_map.at(rhs_type) = std::move(temp);
    }
  }
  return *this;
}

std::unordered_map<std::string, std::set<SlotId>>&
DedicatedResourceInNode::operator[](const std::string& device_name) {
  return this->name_type_slots_map[device_name];
}
bool DedicatedResourceInNode::contains(const std::string& device_name) const {
  return this->name_type_slots_map.contains(device_name);
}
