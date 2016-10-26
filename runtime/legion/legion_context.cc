/* Copyright 2016 Stanford University, NVIDIA Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime.h"
#include "legion_tasks.h"
#include "legion_trace.h"
#include "legion_context.h"
#include "legion_instances.h"
#include "legion_views.h"

namespace Legion {
  namespace Internal {

    LEGION_EXTERN_LOGGER_DECLARATIONS

    /////////////////////////////////////////////////////////////
    // Task Context 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    TaskContext::TaskContext(Runtime *rt, TaskOp *owner,
                             const std::vector<RegionRequirement> &reqs)
      : runtime(rt), owner_task(owner), regions(reqs),
        executing_processor(Processor::NO_PROC), total_tunable_count(0), 
        overhead_tracker(NULL), task_executed(false),
        children_complete_invoked(false), children_commit_invoked(false)
    //--------------------------------------------------------------------------
    {
      context_lock = Reservation::create_reservation();
    }

    //--------------------------------------------------------------------------
    TaskContext::TaskContext(const TaskContext &rhs)
      : runtime(NULL), owner_task(NULL), regions(rhs.regions)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    TaskContext::~TaskContext(void)
    //--------------------------------------------------------------------------
    {
      context_lock.destroy_reservation();
      context_lock = Reservation::NO_RESERVATION;
    }

    //--------------------------------------------------------------------------
    TaskContext& TaskContext::operator=(const TaskContext &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    UniqueID TaskContext::get_context_uid(void) const
    //--------------------------------------------------------------------------
    {
      return owner_task->get_unique_op_id();
    }

    //--------------------------------------------------------------------------
    int TaskContext::get_depth(void) const
    //--------------------------------------------------------------------------
    {
      return owner_task->get_depth();
    }

    //--------------------------------------------------------------------------
    Task* TaskContext::get_task(void)
    //--------------------------------------------------------------------------
    {
      return owner_task;
    }

    //--------------------------------------------------------------------------
    bool TaskContext::is_leaf_context(void) const
    //--------------------------------------------------------------------------
    {
      return false;
    }

    //--------------------------------------------------------------------------
    void TaskContext::add_physical_region(const RegionRequirement &req,
                                   bool mapped, MapperID mid, MappingTagID tag,
                                   ApUserEvent unmap_event, bool virtual_mapped, 
                                   const InstanceSet &physical_instances)
    //--------------------------------------------------------------------------
    {
      PhysicalRegionImpl *impl = new PhysicalRegionImpl(req, 
          ApEvent::NO_AP_EVENT, mapped, this, mid, tag, 
          is_leaf_context(), virtual_mapped, runtime);
      physical_regions.push_back(PhysicalRegion(impl));
      if (mapped)
        impl->reset_references(physical_instances, unmap_event);
    }

    //--------------------------------------------------------------------------
    PhysicalRegion TaskContext::get_physical_region(unsigned idx)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(idx < physical_regions.size());
#endif
      return physical_regions[idx];
    } 

    //--------------------------------------------------------------------------
    void TaskContext::destroy_user_lock(Reservation r)
    //--------------------------------------------------------------------------
    {
      // Can only be called from user land so no
      // need to hold the lock
      context_locks.push_back(r);
    }

    //--------------------------------------------------------------------------
    void TaskContext::destroy_user_barrier(ApBarrier b)
    //--------------------------------------------------------------------------
    {
      // Can only be called from user land so no 
      // need to hold the lock
      context_barriers.push_back(b);
    } 

    //--------------------------------------------------------------------------
    void TaskContext::add_local_field(FieldSpace handle, FieldID fid, 
                                    size_t field_size, CustomSerdezID serdez_id)
    //--------------------------------------------------------------------------
    {
      allocate_local_field(local_fields.back());
      // Hold the lock when modifying the local_fields data structure
      // since it can be read by tasks that are being packed
      ApEvent completion_event = owner_task->get_task_completion(); 
      AutoLock ctx_lock(context_lock);
      local_fields.push_back(
          LocalFieldInfo(handle, fid, field_size, 
            Runtime::protect_event(completion_event), serdez_id));
    }

    //--------------------------------------------------------------------------
    void TaskContext::add_local_fields(FieldSpace handle,
                                      const std::vector<FieldID> &fields,
                                      const std::vector<size_t> &field_sizes,
                                      CustomSerdezID serdez_id)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(fields.size() == field_sizes.size());
#endif
      for (unsigned idx = 0; idx < fields.size(); idx++)
        add_local_field(handle, fields[idx], field_sizes[idx], serdez_id);
    }

    //--------------------------------------------------------------------------
    void TaskContext::allocate_local_field(const LocalFieldInfo &info)
    //--------------------------------------------------------------------------
    {
      // Try allocating a local field and if we succeeded then launch
      // a deferred task to reclaim the field whenever it's completion
      // event has triggered.  Otherwise it already exists on this node
      // so we are free to use it no matter what
      if (runtime->forest->allocate_field(info.handle, info.field_size,
                                       info.fid, info.serdez_id, true/*local*/))
      {
        ReclaimLocalFieldArgs args;
        args.handle = info.handle;
        args.fid = info.fid;
        runtime->issue_runtime_meta_task(args, LG_LATENCY_PRIORITY,
                                         owner_task, info.reclaim_event);
      }
    }

    //--------------------------------------------------------------------------
    ptr_t TaskContext::perform_safe_cast(IndexSpace handle, ptr_t pointer)
    //--------------------------------------------------------------------------
    {
      DomainPoint point(pointer.value);
      std::map<IndexSpace,Domain>::const_iterator finder = 
                                              safe_cast_domains.find(handle);
      if (finder != safe_cast_domains.end())
      {
        if (finder->second.contains(point))
          return pointer;
        else
          return ptr_t::nil();
      }
      Domain domain = runtime->get_index_space_domain(this, handle);
      // Save the result
      safe_cast_domains[handle] = domain;
      if (domain.contains(point))
        return pointer;
      else
        return ptr_t::nil();
    }
    
    //--------------------------------------------------------------------------
    DomainPoint TaskContext::perform_safe_cast(IndexSpace handle, 
                                              const DomainPoint &point)
    //--------------------------------------------------------------------------
    {
      std::map<IndexSpace,Domain>::const_iterator finder = 
                                              safe_cast_domains.find(handle);
      if (finder != safe_cast_domains.end())
      {
        if (finder->second.contains(point))
          return point;
        else
          return DomainPoint::nil();
      }
      Domain domain = runtime->get_index_space_domain(this, handle);
      // Save the result
      safe_cast_domains[handle] = domain;
      if (domain.contains(point))
        return point;
      else
        return DomainPoint::nil();
    }

    //--------------------------------------------------------------------------
    void TaskContext::add_created_region(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      // Already hold the lock from the caller
      RegionRequirement new_req(handle, READ_WRITE, EXCLUSIVE, handle);
      // Put a region requirement with no fields in the list of
      // created requirements, we know we can add any fields for
      // this field space in the future since we own all privileges
      // Now make a new region requirement and physical region
      created_requirements.push_back(new_req);
      // Created regions always return privileges that they make
      returnable_privileges.push_back(true);
      // Make a new unmapped physical region if we aren't done executing yet
      if (!task_executed)
        physical_regions.push_back(PhysicalRegion(
              legion_new<PhysicalRegionImpl>(created_requirements.back(), 
                ApEvent::NO_AP_EVENT, false/*mapped*/, this, 
                owner_task->map_id, owner_task->tag, 
                is_leaf_context(), false/*virtual mapped*/, runtime)));
    }

    //--------------------------------------------------------------------------
    void TaskContext::log_created_requirements(void)
    //--------------------------------------------------------------------------
    {
      std::vector<MappingInstance> instances(1, 
            Mapping::PhysicalInstance::get_virtual_instance());
      const UniqueID unique_op_id = get_unique_id();
      const size_t original_size = owner_task->regions.size();
      for (unsigned idx = 0; idx < created_requirements.size(); idx++)
      {
        // Skip it if there are no privilege fields
        if (created_requirements[idx].privilege_fields.empty())
          continue;
        TaskOp::log_requirement(unique_op_id, original_size + idx, 
                                created_requirements[idx]);
        InstanceSet instance_set;
        std::vector<PhysicalManager*> unacquired;  
        RegionTreeID bad_tree; std::vector<FieldID> missing_fields;
        runtime->forest->physical_convert_mapping(owner_task, 
            created_requirements[idx], instances, instance_set, bad_tree, 
            missing_fields, NULL, unacquired, false/*do acquire_checks*/);
        runtime->forest->log_mapping_decision(unique_op_id,
            original_size + idx, created_requirements[idx], instance_set);
      }
    } 

    //--------------------------------------------------------------------------
    void TaskContext::register_region_creation(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      // Create a new logical region 
      // Hold the operation lock when doing this since children could
      // be returning values from the utility processor
      AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
      assert(created_regions.find(handle) == created_regions.end());
#endif
      created_regions.insert(handle); 
      add_created_region(handle);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_region_deletion(LogicalRegion handle)
    //--------------------------------------------------------------------------
    {
      bool finalize = false;
      // Hold the operation lock when doing this since children could
      // be returning values from the utility processor
      {
        AutoLock ctx_lock(context_lock);
        std::set<LogicalRegion>::iterator finder = created_regions.find(handle);
        // See if we created this region, if so remove it from the list
        // of created regions, otherwise add it to the list of deleted
        // regions to flow backwards
        if (finder != created_regions.end())
        {
          created_regions.erase(finder);
          finalize = true;
        }
        else
          deleted_regions.insert(handle);
      }
      if (finalize)
        runtime->finalize_logical_region_destroy(handle);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_creation(FieldSpace handle, FieldID fid)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      std::pair<FieldSpace,FieldID> key(handle,fid);
#ifdef DEBUG_LEGION
      assert(created_fields.find(key) == created_fields.end());
#endif
      created_fields.insert(key);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_creations(FieldSpace handle,
                                          const std::vector<FieldID> &fields)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      for (unsigned idx = 0; idx < fields.size(); idx++)
      {
        std::pair<FieldSpace,FieldID> key(handle,fields[idx]);
#ifdef DEBUG_LEGION
        assert(created_fields.find(key) == created_fields.end());
#endif
        created_fields.insert(key);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_deletions(FieldSpace handle,
                                         const std::set<FieldID> &to_free)
    //--------------------------------------------------------------------------
    {
      std::set<FieldID> to_finalize;
      {
        AutoLock ctx_lock(context_lock);
        for (std::set<FieldID>::const_iterator it = to_free.begin();
              it != to_free.end(); it++)
        {
          std::pair<FieldSpace,FieldID> key(handle,*it);
          std::set<std::pair<FieldSpace,FieldID> >::iterator finder = 
            created_fields.find(key);
          if (finder != created_fields.end())
          {
            created_fields.erase(finder);
            to_finalize.insert(*it);
          }
          else
            deleted_fields.insert(key);
        }
      }
      if (!to_finalize.empty())
        runtime->finalize_field_destroy(handle, to_finalize);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_space_creation(FieldSpace space)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
      assert(created_field_spaces.find(space) == created_field_spaces.end());
#endif
      created_field_spaces.insert(space);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_space_deletion(FieldSpace space)
    //--------------------------------------------------------------------------
    {
      bool finalize = false;
      {
        AutoLock ctx_lock(context_lock);
        std::deque<FieldID> to_delete;
        for (std::set<std::pair<FieldSpace,FieldID> >::const_iterator it = 
              created_fields.begin(); it != created_fields.end(); it++)
        {
          if (it->first == space)
            to_delete.push_back(it->second);
        }
        for (unsigned idx = 0; idx < to_delete.size(); idx++)
        {
          std::pair<FieldSpace,FieldID> key(space, to_delete[idx]);
          created_fields.erase(key);
        }
        std::set<FieldSpace>::iterator finder = 
          created_field_spaces.find(space);
        if (finder != created_field_spaces.end())
        {
          created_field_spaces.erase(finder);
          finalize = true;
        }
        else
          deleted_field_spaces.insert(space);
      }
      if (finalize)
        runtime->finalize_field_space_destroy(space);
    }

    //--------------------------------------------------------------------------
    bool TaskContext::has_created_index_space(IndexSpace space) const
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      return (created_index_spaces.find(space) != created_index_spaces.end());
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_space_creation(IndexSpace space)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
      assert(created_index_spaces.find(space) == created_index_spaces.end());
#endif
      created_index_spaces.insert(space);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_space_deletion(IndexSpace space)
    //--------------------------------------------------------------------------
    {
      bool finalize = false;
      {
        AutoLock ctx_lock(context_lock);
        std::set<IndexSpace>::iterator finder = 
          created_index_spaces.find(space);
        if (finder != created_index_spaces.end())
        {
          created_index_spaces.erase(finder);
          finalize = true;
        }
        else
          deleted_index_spaces.insert(space);
      }
      if (finalize)
        runtime->finalize_index_space_destroy(space);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_partition_creation(IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
      assert(created_index_partitions.find(handle) == 
             created_index_partitions.end());
#endif
      created_index_partitions.insert(handle);
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_partition_deletion(IndexPartition handle)
    //--------------------------------------------------------------------------
    {
      bool finalize = false;
      {
        AutoLock ctx_lock(context_lock);
        std::set<IndexPartition>::iterator finder = 
          created_index_partitions.find(handle);
        if (finder != created_index_partitions.end())
        {
          created_index_partitions.erase(finder);
          finalize = true;
        }
        else
          deleted_index_partitions.insert(handle);
      }
      if (finalize)
        runtime->finalize_index_partition_destroy(handle);
    }

    //--------------------------------------------------------------------------
    bool TaskContext::was_created_requirement_deleted(
                                             const RegionRequirement &req) const
    //--------------------------------------------------------------------------
    {
      // Region was created and not deleted
      if (created_regions.find(req.region) != created_regions.end())
        return false;
      // Otherwise see if the field was created and still not deleted
      // If it is has more than one privilege field then it was not 
      // a created field
      if (req.privilege_fields.size() > 1)
        return true;
      std::pair<FieldSpace,FieldID> key(req.region.get_field_space(),
                                    *(req.privilege_fields.begin()));
      if (created_fields.find(key) != created_fields.end())
        return false;
      return true;
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_region_creations(
                                            const std::set<LogicalRegion> &regs)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      for (std::set<LogicalRegion>::const_iterator it = regs.begin();
            it != regs.end(); it++)
      {
#ifdef DEBUG_LEGION
        assert(created_regions.find(*it) == created_regions.end());
#endif
        created_regions.insert(*it);
        add_created_region(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_region_deletions(
                                            const std::set<LogicalRegion> &regs)
    //--------------------------------------------------------------------------
    {
      std::vector<LogicalRegion> to_finalize;
      {
        AutoLock ctx_lock(context_lock);
        for (std::set<LogicalRegion>::const_iterator it = regs.begin();
              it != regs.end(); it++)
        {
          std::set<LogicalRegion>::iterator finder = created_regions.find(*it);
          if (finder != created_regions.end())
          {
            created_regions.erase(finder);
            to_finalize.push_back(*it);
          }
          else
            deleted_regions.insert(*it);
        }
      }
      if (!to_finalize.empty())
      {
        for (std::vector<LogicalRegion>::const_iterator it = 
              to_finalize.begin(); it != to_finalize.end(); it++)
          runtime->finalize_logical_region_destroy(*it);
      }
    } 

    //--------------------------------------------------------------------------
    void TaskContext::register_field_creations(
                         const std::set<std::pair<FieldSpace,FieldID> > &fields)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      for (std::set<std::pair<FieldSpace,FieldID> >::const_iterator it = 
            fields.begin(); it != fields.end(); it++)
      {
#ifdef DEBUG_LEGION
        assert(created_fields.find(*it) == created_fields.end());
#endif
        created_fields.insert(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_deletions(
                        const std::set<std::pair<FieldSpace,FieldID> > &fields)
    //--------------------------------------------------------------------------
    {
      std::map<FieldSpace,std::set<FieldID> > to_finalize;
      {
        AutoLock ctx_lock(context_lock);
        for (std::set<std::pair<FieldSpace,FieldID> >::const_iterator it = 
              fields.begin(); it != fields.end(); it++)
        {
          std::set<std::pair<FieldSpace,FieldID> >::iterator finder = 
            created_fields.find(*it);
          if (finder != created_fields.end())
          {
            created_fields.erase(finder);
            to_finalize[it->first].insert(it->second);
          }
          else
            deleted_fields.insert(*it);
        }
      }
      if (!to_finalize.empty())
      {
        for (std::map<FieldSpace,std::set<FieldID> >::const_iterator it = 
              to_finalize.begin(); it != to_finalize.end(); it++)
        {
          runtime->finalize_field_destroy(it->first, it->second);
        }
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_space_creations(
                                            const std::set<FieldSpace> &spaces)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      for (std::set<FieldSpace>::const_iterator it = spaces.begin();
            it != spaces.end(); it++)
      {
#ifdef DEBUG_LEGION
        assert(created_field_spaces.find(*it) == created_field_spaces.end());
#endif
        created_field_spaces.insert(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_field_space_deletions(
                                            const std::set<FieldSpace> &spaces)
    //--------------------------------------------------------------------------
    {
      std::vector<FieldSpace> to_finalize;
      {
        AutoLock ctx_lock(context_lock);
        for (std::set<FieldSpace>::const_iterator it = spaces.begin();
              it != spaces.end(); it++)
        {
          std::deque<FieldID> to_delete;
          for (std::set<std::pair<FieldSpace,FieldID> >::const_iterator cit = 
                created_fields.begin(); cit != created_fields.end(); cit++)
          {
            if (cit->first == *it)
              to_delete.push_back(cit->second);
          }
          for (unsigned idx = 0; idx < to_delete.size(); idx++)
          {
            std::pair<FieldSpace,FieldID> key(*it, to_delete[idx]);
            created_fields.erase(key);
          }
          std::set<FieldSpace>::iterator finder = created_field_spaces.find(*it);
          if (finder != created_field_spaces.end())
          {
            created_field_spaces.erase(finder);
            to_finalize.push_back(*it);
          }
          else
            deleted_field_spaces.insert(*it);
        }
      }
      if (!to_finalize.empty())
      {
        for (std::vector<FieldSpace>::const_iterator it = to_finalize.begin();
              it != to_finalize.end(); it++)
          runtime->finalize_field_space_destroy(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_space_creations(
                                            const std::set<IndexSpace> &spaces)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      for (std::set<IndexSpace>::const_iterator it = spaces.begin();
            it != spaces.end(); it++)
      {
#ifdef DEBUG_LEGION
        assert(created_index_spaces.find(*it) == created_index_spaces.end());
#endif
        created_index_spaces.insert(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_space_deletions(
                                            const std::set<IndexSpace> &spaces)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexSpace> to_finalize;
      {
        AutoLock ctx_lock(context_lock);
        for (std::set<IndexSpace>::const_iterator it = spaces.begin();
              it != spaces.end(); it++)
        {
          std::set<IndexSpace>::iterator finder = 
            created_index_spaces.find(*it);
          if (finder != created_index_spaces.end())
          {
            created_index_spaces.erase(finder);
            to_finalize.push_back(*it);
          }
          else
            deleted_index_spaces.insert(*it);
        }
      }
      if (!to_finalize.empty())
      {
        for (std::vector<IndexSpace>::const_iterator it = to_finalize.begin();
              it != to_finalize.end(); it++)
          runtime->finalize_index_space_destroy(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_partition_creations(
                                          const std::set<IndexPartition> &parts)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      for (std::set<IndexPartition>::const_iterator it = parts.begin();
            it != parts.end(); it++)
      {
#ifdef DEBUG_LEGION
        assert(created_index_partitions.find(*it) == 
               created_index_partitions.end());
#endif
        created_index_partitions.insert(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_index_partition_deletions(
                                          const std::set<IndexPartition> &parts)
    //--------------------------------------------------------------------------
    {
      std::vector<IndexPartition> to_finalize;
      {
        AutoLock ctx_lock(context_lock);
        for (std::set<IndexPartition>::const_iterator it = parts.begin();
              it != parts.end(); it++)
        {
          std::set<IndexPartition>::iterator finder = 
            created_index_partitions.find(*it);
          if (finder != created_index_partitions.end())
          {
            created_index_partitions.erase(finder);
            to_finalize.push_back(*it);
          }
          else
            deleted_index_partitions.insert(*it);
        }
      }
      if (!to_finalize.empty())
      {
        for (std::vector<IndexPartition>::const_iterator it = 
              to_finalize.begin(); it != to_finalize.end(); it++)
          runtime->finalize_index_partition_destroy(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::analyze_destroy_index_space(IndexSpace handle,
                                    std::vector<RegionRequirement> &delete_reqs,
                                    std::vector<unsigned> &parent_req_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_leaf_context());
#endif
      // Iterate through our region requirements and find the
      // ones we interfere with
      unsigned parent_index = 0;
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.index_space.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(handle, it->region.index_space))
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_index_path(it->region.index_space, 
                                                handle, dummy_path))
          req.region = LogicalRegion(it->region.get_tree_id(), handle, 
                                     it->region.get_field_space());
        else
          req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
      // Now do the same thing for the created requirements
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (std::deque<RegionRequirement>::const_iterator it = 
            created_requirements.begin(); it != 
            created_requirements.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.index_space.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(handle, it->region.index_space))
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_index_path(it->region.index_space,
                                                handle, dummy_path))
          req.region = LogicalRegion(it->region.get_tree_id(), handle, 
                                     it->region.get_field_space());
        else
          req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::analyze_destroy_index_partition(IndexPartition handle,
                                    std::vector<RegionRequirement> &delete_reqs,
                                    std::vector<unsigned> &parent_req_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_leaf_context());
#endif
      // Iterate through our region requirements and find the
      // ones we interfere with
      unsigned parent_index = 0;
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.index_space.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(it->region.index_space, handle))
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_partition_path(it->region.index_space,
                                                    handle, dummy_path))
        {
          req.partition = LogicalPartition(it->region.get_tree_id(), handle,
                                           it->region.get_field_space());
          req.handle_type = PART_PROJECTION;
        }
        else
        {
          req.region = it->region;
          req.handle_type = SINGULAR;
        }
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        parent_req_indexes.push_back(parent_index);
      }
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (std::deque<RegionRequirement>::const_iterator it = 
            created_requirements.begin(); it != 
            created_requirements.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.index_space.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(it->region.index_space, handle))
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_partition_path(it->region.index_space,
                                                    handle, dummy_path))
        {
          req.partition = LogicalPartition(it->region.get_tree_id(), handle,
                                           it->region.get_field_space());
          req.handle_type = PART_PROJECTION;
        }
        else
        {
          req.region = it->region;
          req.handle_type = SINGULAR;
        }
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        parent_req_indexes.push_back(parent_index);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::analyze_destroy_field_space(FieldSpace handle,
                                    std::vector<RegionRequirement> &delete_reqs,
                                    std::vector<unsigned> &parent_req_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_leaf_context());
#endif
      unsigned parent_index = 0;
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++, parent_index++)
      {
        if (it->region.get_field_space() != handle)
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (std::deque<RegionRequirement>::const_iterator it = 
            created_requirements.begin(); it != 
            created_requirements.end(); it++, parent_index++)
      {
        if (it->region.get_field_space() != handle)
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::analyze_destroy_fields(FieldSpace handle,
                                             const std::set<FieldID> &to_delete,
                                    std::vector<RegionRequirement> &delete_reqs,
                                    std::vector<unsigned> &parent_req_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_leaf_context());
#endif
      unsigned parent_index = 0;
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++, parent_index++)
      {
        if (it->region.get_field_space() != handle)
          continue;
        std::set<FieldID> overlapping_fields;
        for (std::set<FieldID>::const_iterator fit = to_delete.begin();
              fit != to_delete.end(); fit++)
        {
          if (it->privilege_fields.find(*fit) != it->privilege_fields.end())
            overlapping_fields.insert(*fit);
        }
        if (overlapping_fields.empty())
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = overlapping_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (std::deque<RegionRequirement>::const_iterator it = 
            created_requirements.begin(); it != 
            created_requirements.end(); it++, parent_index++)
      {
        if (it->region.get_field_space() != handle)
          continue;
        std::set<FieldID> overlapping_fields;
        for (std::set<FieldID>::const_iterator fit = to_delete.begin();
              fit != to_delete.end(); fit++)
        {
          if (it->privilege_fields.find(*fit) != it->privilege_fields.end())
            overlapping_fields.insert(*fit);
        }
        if (overlapping_fields.empty())
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = overlapping_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::analyze_destroy_logical_region(LogicalRegion handle,
                                    std::vector<RegionRequirement> &delete_reqs,
                                    std::vector<unsigned> &parent_req_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_leaf_context());
#endif
      unsigned parent_index = 0;
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(handle.get_index_space(), 
                                          it->region.index_space))
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_index_path(it->region.index_space,
                                  handle.get_index_space(), dummy_path))
          req.region = handle;
        else
          req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (std::deque<RegionRequirement>::const_iterator it = 
            created_requirements.begin(); it != 
            created_requirements.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(handle.get_index_space(), 
                                          it->region.index_space))
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_index_path(it->region.index_space,
                                  handle.get_index_space(), dummy_path))
          req.region = handle;
        else
          req.region = it->region;
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        req.handle_type = SINGULAR;
        parent_req_indexes.push_back(parent_index);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::analyze_destroy_logical_partition(LogicalPartition handle,
                                    std::vector<RegionRequirement> &delete_reqs,
                                    std::vector<unsigned> &parent_req_indexes)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!is_leaf_context());
#endif
      unsigned parent_index = 0;
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(it->region.index_space,
                                          handle.get_index_partition())) 
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_partition_path(it->region.index_space,
                                  handle.get_index_partition(), dummy_path))
        {
          req.partition = handle;
          req.handle_type = PART_PROJECTION;
        }
        else
        {
          req.region = it->region;
          req.handle_type = SINGULAR;
        }
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        parent_req_indexes.push_back(parent_index);
      }
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (std::deque<RegionRequirement>::const_iterator it = 
            created_requirements.begin(); it != 
            created_requirements.end(); it++, parent_index++)
      {
        // Different index space trees means we can skip
        if (handle.get_tree_id() != it->region.get_tree_id())
          continue;
        // Disjoint index spaces means we can skip
        if (runtime->forest->are_disjoint(it->region.index_space,
                                          handle.get_index_partition())) 
          continue;
        delete_reqs.resize(delete_reqs.size()+1);
        RegionRequirement &req = delete_reqs.back();
        std::vector<ColorPoint> dummy_path;
        // See if we dominate the deleted instance
        if (runtime->forest->compute_partition_path(it->region.index_space,
                                  handle.get_index_partition(), dummy_path))
        {
          req.partition = handle;
          req.handle_type = PART_PROJECTION;
        }
        else
        {
          req.region = it->region;
          req.handle_type = SINGULAR;
        }
        req.parent = it->region;
        req.privilege = READ_WRITE;
        req.prop = EXCLUSIVE;
        req.privilege_fields = it->privilege_fields;
        parent_req_indexes.push_back(parent_index);
      }
    }

    //--------------------------------------------------------------------------
    int TaskContext::has_conflicting_regions(MapOp *op, bool &parent_conflict,
                                             bool &inline_conflict)
    //--------------------------------------------------------------------------
    {
      const RegionRequirement &req = op->get_requirement(); 
      return has_conflicting_internal(req, parent_conflict, inline_conflict);
    }

    //--------------------------------------------------------------------------
    int TaskContext::has_conflicting_regions(AttachOp *attach,
                                             bool &parent_conflict,
                                             bool &inline_conflict)
    //--------------------------------------------------------------------------
    {
      const RegionRequirement &req = attach->get_requirement();
      return has_conflicting_internal(req, parent_conflict, inline_conflict);
    }

    //--------------------------------------------------------------------------
    int TaskContext::has_conflicting_internal(const RegionRequirement &req,
                                              bool &parent_conflict,
                                              bool &inline_conflict)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, HAS_CONFLICTING_INTERNAL_CALL);
      parent_conflict = false;
      inline_conflict = false;
      // No need to hold our lock here because we are the only ones who
      // could possibly be doing any mutating of the physical_regions data 
      // structure but we are here so we aren't mutating
      for (unsigned our_idx = 0; our_idx < physical_regions.size(); our_idx++)
      {
        // skip any regions which are not mapped
        if (!physical_regions[our_idx].impl->is_mapped())
          continue;
        const RegionRequirement &our_req = 
          physical_regions[our_idx].impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
        {
          parent_conflict = true;
          return our_idx;
        }
      }
      for (std::list<PhysicalRegion>::const_iterator it = 
            inline_regions.begin(); it != inline_regions.end(); it++)
      {
        if (!it->impl->is_mapped())
          continue;
        const RegionRequirement &our_req = it->impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
        {
          inline_conflict = true;
          // No index for inline conflicts
          return -1;
        }
      }
      return -1;
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_regions(TaskOp *task,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      // No need to hold our lock here because we are the only ones who
      // could possibly be doing any mutating of the physical_regions data 
      // structure but we are here so we aren't mutating
      for (unsigned our_idx = 0; our_idx < physical_regions.size(); our_idx++)
      {
        // Skip any regions which are not mapped
        if (!physical_regions[our_idx].impl->is_mapped())
          continue;
        const RegionRequirement &our_req = 
          physical_regions[our_idx].impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        // Check to see if any region requirements from the child have
        // a dependence on our region at location our_idx
        for (unsigned idx = 0; idx < task->regions.size(); idx++)
        {
          const RegionRequirement &req = task->regions[idx];  
          if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
          {
            conflicting.push_back(physical_regions[our_idx]);
            // Once we find a conflict, we don't need to check
            // against it anymore, so go onto our next region
            break;
          }
        }
      }
      for (std::list<PhysicalRegion>::const_iterator it = 
            inline_regions.begin(); it != inline_regions.end(); it++)
      {
        if (!it->impl->is_mapped())
          continue;
        const RegionRequirement &our_req = it->impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        // Check to see if any region requirements from the child have
        // a dependence on our region at location our_idx
        for (unsigned idx = 0; idx < task->regions.size(); idx++)
        {
          const RegionRequirement &req = task->regions[idx];  
          if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
          {
            conflicting.push_back(*it);
            // Once we find a conflict, we don't need to check
            // against it anymore, so go onto our next region
            break;
          }
        }
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_regions(CopyOp *copy,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      // No need to hold our lock here because we are the only ones who
      // could possibly be doing any mutating of the physical_regions data 
      // structure but we are here so we aren't mutating
      for (unsigned our_idx = 0; our_idx < physical_regions.size(); our_idx++)
      {
        // skip any regions which are not mapped
        if (!physical_regions[our_idx].impl->is_mapped())
          continue;
        const RegionRequirement &our_req = 
          physical_regions[our_idx].impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        bool has_conflict = false;
        for (unsigned idx = 0; !has_conflict &&
              (idx < copy->src_requirements.size()); idx++)
        {
          const RegionRequirement &req = copy->src_requirements[idx];
          if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
            has_conflict = true;
        }
        for (unsigned idx = 0; !has_conflict &&
              (idx < copy->dst_requirements.size()); idx++)
        {
          const RegionRequirement &req = copy->dst_requirements[idx];
          if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
            has_conflict = true;
        }
        if (has_conflict)
          conflicting.push_back(physical_regions[our_idx]);
      }
      for (std::list<PhysicalRegion>::const_iterator it = 
            inline_regions.begin(); it != inline_regions.end(); it++)
      {
        if (!it->impl->is_mapped())
          continue;
        const RegionRequirement &our_req = it->impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        bool has_conflict = false;
        for (unsigned idx = 0; !has_conflict &&
              (idx < copy->src_requirements.size()); idx++)
        {
          const RegionRequirement &req = copy->src_requirements[idx];
          if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
            has_conflict = true;
        }
        for (unsigned idx = 0; !has_conflict &&
              (idx < copy->dst_requirements.size()); idx++)
        {
          const RegionRequirement &req = copy->dst_requirements[idx];
          if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
            has_conflict = true;
        }
        if (has_conflict)
          conflicting.push_back(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_regions(AcquireOp *acquire,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      const RegionRequirement &req = acquire->get_requirement();
      find_conflicting_internal(req, conflicting); 
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_regions(ReleaseOp *release,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      const RegionRequirement &req = release->get_requirement();
      find_conflicting_internal(req, conflicting);      
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_regions(DependentPartitionOp *partition,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      const RegionRequirement &req = partition->get_requirement();
      find_conflicting_internal(req, conflicting);
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_internal(const RegionRequirement &req,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      // No need to hold our lock here because we are the only ones who
      // could possibly be doing any mutating of the physical_regions data 
      // structure but we are here so we aren't mutating
      for (unsigned our_idx = 0; our_idx < physical_regions.size(); our_idx++)
      {
        // skip any regions which are not mapped
        if (!physical_regions[our_idx].impl->is_mapped())
          continue;
        const RegionRequirement &our_req = 
          physical_regions[our_idx].impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
          conflicting.push_back(physical_regions[our_idx]);
      }
      for (std::list<PhysicalRegion>::const_iterator it = 
            inline_regions.begin(); it != inline_regions.end(); it++)
      {
        if (!it->impl->is_mapped())
          continue;
        const RegionRequirement &our_req = it->impl->get_requirement();
#ifdef DEBUG_LEGION
        // This better be true for a single task
        assert(our_req.handle_type == SINGULAR);
#endif
        RegionTreeID our_tid = our_req.region.get_tree_id();
        IndexSpace our_space = our_req.region.get_index_space();
        RegionUsage our_usage(our_req);
        if (check_region_dependence(our_tid,our_space,our_req,our_usage,req))
          conflicting.push_back(*it);
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_conflicting_regions(FillOp *fill,
                                       std::vector<PhysicalRegion> &conflicting)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_CONFLICTING_CALL);
      const RegionRequirement &req = fill->get_requirement();
      find_conflicting_internal(req, conflicting);
    }

    //--------------------------------------------------------------------------
    bool TaskContext::check_region_dependence(RegionTreeID our_tid,
                                             IndexSpace our_space,
                                             const RegionRequirement &our_req,
                                             const RegionUsage &our_usage,
                                             const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, CHECK_REGION_DEPENDENCE_CALL);
      if ((req.handle_type == SINGULAR) || 
          (req.handle_type == REG_PROJECTION))
      {
        // If the trees are different we're done 
        if (our_tid != req.region.get_tree_id())
          return false;
        // Check to see if there is a path between
        // the index spaces
        std::vector<ColorPoint> path;
        if (!runtime->forest->compute_index_path(our_space,
                         req.region.get_index_space(),path))
          return false;
      }
      else
      {
        // Check if the trees are different
        if (our_tid != req.partition.get_tree_id())
          return false;
        std::vector<ColorPoint> path;
        if (!runtime->forest->compute_partition_path(our_space,
                     req.partition.get_index_partition(), path))
          return false;
      }
      // Check to see if any privilege fields overlap
      std::vector<FieldID> intersection(our_req.privilege_fields.size());
      std::vector<FieldID>::iterator intersect_it = 
        std::set_intersection(our_req.privilege_fields.begin(),
                              our_req.privilege_fields.end(),
                              req.privilege_fields.begin(),
                              req.privilege_fields.end(),
                              intersection.begin());
      intersection.resize(intersect_it - intersection.begin());
      if (intersection.empty())
        return false;
      // Finally if everything has overlapped, do a dependence analysis
      // on the privileges and coherence
      RegionUsage usage(req);
      switch (check_dependence_type(our_usage,usage))
      {
        // Only allow no-dependence, or simultaneous dependence through
        case NO_DEPENDENCE:
        case SIMULTANEOUS_DEPENDENCE:
          {
            return false;
          }
        default:
          break;
      }
      return true;
    }

    //--------------------------------------------------------------------------
    void TaskContext::register_inline_mapped_region(PhysicalRegion &region)
    //--------------------------------------------------------------------------
    {
      // Don't need the lock because this is only accessed from 
      // the executing task context
      //
      // Because of 'remap_region', this method can be called
      // both for inline regions as well as regions which were
      // initally mapped for the task.  Do a quick check to see
      // if it was an original region.  If it was then we're done.
      for (unsigned idx = 0; idx < physical_regions.size(); idx++)
      {
        if (physical_regions[idx].impl == region.impl)
          return;
      }
      inline_regions.push_back(region);
    }

    //--------------------------------------------------------------------------
    void TaskContext::unregister_inline_mapped_region(PhysicalRegion &region)
    //--------------------------------------------------------------------------
    {
      // Don't need the lock because this is only accessed from the
      // executed task context
      for (std::list<PhysicalRegion>::iterator it = 
            inline_regions.begin(); it != inline_regions.end(); it++)
      {
        if (it->impl == region.impl)
        {
          inline_regions.erase(it);
          return;
        }
      }
    }

    //--------------------------------------------------------------------------
    bool TaskContext::is_region_mapped(unsigned idx)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
      assert(idx < physical_regions.size());
#endif
      return physical_regions[idx].impl->is_mapped();
    }

    //--------------------------------------------------------------------------
    void TaskContext::clone_requirement(unsigned idx, RegionRequirement &target)
    //--------------------------------------------------------------------------
    {
      if (idx >= regions.size())
      {
        idx -= regions.size();
        AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
        assert(idx < created_requirements.size());
#endif
        target = created_requirements[idx];
      }
      else
        target = regions[idx];
    }

    //--------------------------------------------------------------------------
    int TaskContext::find_parent_region_req(const RegionRequirement &req,
                                            bool check_privilege /*= true*/)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_PARENT_REGION_REQ_CALL);
      // We can check most of our region requirements without the lock
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        const RegionRequirement &our_req = regions[idx];
        // First check that the regions match
        if (our_req.region != req.parent)
          continue;
        // Next check the privileges
        if (check_privilege && 
            ((req.privilege & our_req.privilege) != req.privilege))
          continue;
        // Finally check that all the fields are contained
        bool dominated = true;
        for (std::set<FieldID>::const_iterator it = 
              req.privilege_fields.begin(); it !=
              req.privilege_fields.end(); it++)
        {
          if (our_req.privilege_fields.find(*it) ==
              our_req.privilege_fields.end())
          {
            dominated = false;
            break;
          }
        }
        if (!dominated)
          continue;
        return int(idx);
      }
      const FieldSpace fs = req.parent.get_field_space(); 
      // The created region requirements have to be checked while holding
      // the lock since they are subject to mutation by the application
      // We might also mutate it so we take the lock in exclusive mode
      AutoLock ctx_lock(context_lock);
      for (unsigned idx = 0; idx < created_requirements.size(); idx++)
      {
        RegionRequirement &our_req = created_requirements[idx];
        // First check that the regions match
        if (our_req.region != req.parent)
          continue;
        // Next check the privileges
        if (check_privilege && 
            ((req.privilege & our_req.privilege) != req.privilege))
          continue;
        // If this is a returnable privilege requiremnt that means
        // that we made this region so we always have privileges
        // on any fields for that region, just add them and be done
        if (returnable_privileges[idx])
        {
          our_req.privilege_fields.insert(req.privilege_fields.begin(),
                                          req.privilege_fields.end());
          return int(regions.size() + idx);
        }
        // Finally check that all the fields are contained
        bool dominated = true;
        for (std::set<FieldID>::const_iterator it = 
              req.privilege_fields.begin(); it !=
              req.privilege_fields.end(); it++)
        {
          if (our_req.privilege_fields.find(*it) ==
              our_req.privilege_fields.end())
          {
            // Check to see if this is a field we made
            std::pair<FieldSpace,FieldID> key(fs, *it);
            if (created_fields.find(key) != created_fields.end())
            {
              // We made it so we can add it to the requirement
              // and continue on our way
              our_req.privilege_fields.insert(*it);
              continue;
            }
            // Otherwise we don't have privileges
            dominated = false;
            break;
          }
        }
        if (!dominated)
          continue;
        // Include the offset by the number of base requirements
        return int(regions.size() + idx);
      }
      // Method of last resort, check to see if we made all the fields
      // if we did, then we can make a new requirement for all the fields
      for (std::set<FieldID>::const_iterator it = req.privilege_fields.begin();
            it != req.privilege_fields.end(); it++)
      {
        std::pair<FieldSpace,FieldID> key(fs, *it);
        // Didn't make it so we don't have privileges anywhere
        if (created_fields.find(key) == created_fields.end())
          return -1;
      }
      // If we get here then we can make a new requirement
      // which has non-returnable privileges
      // Get the top level region for the region tree
      RegionNode *top = runtime->forest->get_tree(req.parent.get_tree_id());
      RegionRequirement new_req(top->handle, READ_WRITE, EXCLUSIVE,top->handle);
      created_requirements.push_back(new_req);
      // Add our fields
      created_requirements.back().privilege_fields.insert(
          req.privilege_fields.begin(), req.privilege_fields.end());
      // This is not a returnable privilege requirement
      returnable_privileges.push_back(false);
      // Make a new unmapped physical region if we're not done executing yet
      if (!task_executed)
        physical_regions.push_back(PhysicalRegion(
              legion_new<PhysicalRegionImpl>(created_requirements.back(),
                ApEvent::NO_AP_EVENT, false/*mapped*/, this, 
                owner_task->map_id, owner_task->tag, 
                is_leaf_context(), false/*virtual mapped*/, runtime)));
      return int(regions.size() + created_requirements.size() - 1);
    }

    //--------------------------------------------------------------------------
    unsigned TaskContext::find_parent_region(unsigned index, TaskOp *child)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, FIND_PARENT_REGION_CALL);
      // We can check these without the lock
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        if (regions[idx].region == child->regions[index].parent)
          return idx;
      }
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (unsigned idx = 0; idx < created_requirements.size(); idx++)
      {
        if (created_requirements[idx].region == child->regions[index].parent)
          return (regions.size() + idx);
      }
      log_region.error("Parent task %s (ID %lld) of inline task %s "
                        "(ID %lld) does not have a region "
                        "requirement for region (%x,%x,%x) "
                        "as a parent of child task's region "
                        "requirement index %d", get_task_name(),
                        get_unique_id(), child->get_task_name(),
                        child->get_unique_id(), 
                        child->regions[index].region.index_space.id,
                        child->regions[index].region.field_space.id, 
                        child->regions[index].region.tree_id, index);
#ifdef DEBUG_LEGION
      assert(false);
#endif
      exit(ERROR_BAD_PARENT_REGION);
      return 0;
    }

    //--------------------------------------------------------------------------
    unsigned TaskContext::find_parent_index_region(unsigned index,TaskOp *child)
    //--------------------------------------------------------------------------
    {
      for (unsigned idx = 0; idx < owner_task->indexes.size(); idx++)
      {
        if ((owner_task->indexes[idx].handle == child->indexes[idx].parent))
          return idx;
      }
      log_index.error("Parent task %s (ID %lld) of inline task %s "
                            "(ID %lld) does not have an index space "
                            "requirement for index space %x "
                            "as a parent of chlid task's index requirement "
                            "index %d", get_task_name(), get_unique_id(),
                            child->get_task_name(), child->get_unique_id(),
                            child->indexes[index].handle.id, index);
#ifdef DEBUG_LEGION
      assert(false);
#endif
      exit(ERROR_BAD_PARENT_INDEX);
      return 0;
    }

    //--------------------------------------------------------------------------
    PrivilegeMode TaskContext::find_parent_privilege_mode(unsigned idx)
    //--------------------------------------------------------------------------
    {
      if (idx < regions.size())
        return regions[idx].privilege;
      idx -= regions.size();
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
      assert(idx < created_requirements.size());
#endif
      return created_requirements[idx].privilege;
    }

    //--------------------------------------------------------------------------
    LegionErrorType TaskContext::check_privilege(
                                         const IndexSpaceRequirement &req) const
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, CHECK_PRIVILEGE_CALL);
      if (req.verified)
        return NO_ERROR;
      // Find the parent index space
      for (std::vector<IndexSpaceRequirement>::const_iterator it = 
            owner_task->indexes.begin(); it != owner_task->indexes.end(); it++)
      {
        // Check to see if we found the requirement in the parent 
        if (it->handle == req.parent)
        {
          // Check that there is a path between the parent and the child
          std::vector<ColorPoint> path;
          if (!runtime->forest->compute_index_path(req.parent, 
                                                   req.handle, path))
            return ERROR_BAD_INDEX_PATH;
          // Now check that the privileges are less than or equal
          if (req.privilege & (~(it->privilege)))
          {
            return ERROR_BAD_INDEX_PRIVILEGES;  
          }
          return NO_ERROR;
        }
      }
      // If we didn't find it here, we have to check the added 
      // index spaces that we have
      if (has_created_index_space(req.parent))
      {
        // Still need to check that there is a path between the two
        std::vector<ColorPoint> path;
        if (!runtime->forest->compute_index_path(req.parent, req.handle, path))
          return ERROR_BAD_INDEX_PATH;
        // No need to check privileges here since it is a created space
        // which means that the parent has all privileges.
        return NO_ERROR;
      }
      return ERROR_BAD_PARENT_INDEX;
    }

    //--------------------------------------------------------------------------
    LegionErrorType TaskContext::check_privilege(const RegionRequirement &req,
                                                 FieldID &bad_field,
                                                 bool skip_privilege) const
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, CHECK_PRIVILEGE_CALL);
      if (req.flags & VERIFIED_FLAG)
        return NO_ERROR;
      // Copy privilege fields for check
      std::set<FieldID> privilege_fields(req.privilege_fields);
      // Try our original region requirements first
      for (std::vector<RegionRequirement>::const_iterator it = 
            regions.begin(); it != regions.end(); it++)
      {
        LegionErrorType et = 
          check_privilege_internal(req, *it, privilege_fields, bad_field,
                                   skip_privilege);
        // No error so we are done
        if (et == NO_ERROR)
          return et;
        // Something other than bad parent region is a real error
        if (et != ERROR_BAD_PARENT_REGION)
          return et;
        // Otherwise we just keep going
      }
      // If none of that worked, we now get to try the created requirements
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (unsigned idx = 0; idx < created_requirements.size(); idx++)
      {
        LegionErrorType et = 
          check_privilege_internal(req, created_requirements[idx], 
                      privilege_fields, bad_field, skip_privilege);
        // No error so we are done
        if (et == NO_ERROR)
          return et;
        // Something other than bad parent region is a real error
        if (et != ERROR_BAD_PARENT_REGION)
          return et;
        // If we got a BAD_PARENT_REGION, see if this a returnable
        // privilege in which case we know we have privileges on all fields
        if (returnable_privileges[idx])
          return NO_ERROR;
        // Otherwise we just keep going
      }
      // Finally see if we created all the fields in which case we know
      // we have privileges on all their regions
      const FieldSpace sp = req.region.get_field_space();
      for (std::set<FieldID>::const_iterator it = req.privilege_fields.begin();
            it != req.privilege_fields.end(); it++)
      {
        std::pair<FieldSpace,FieldID> key(sp, *it);
        // If we don't find the field, then we are done
        if (created_fields.find(key) == created_fields.end())
          return ERROR_BAD_PARENT_REGION;
      }
      // Otherwise we have privileges on these fields for all regions
      // so we are good on privileges
      return NO_ERROR;
    }

    //--------------------------------------------------------------------------
    LegionErrorType TaskContext::check_privilege_internal(
        const RegionRequirement &req, const RegionRequirement &our_req,
        std::set<FieldID>& privilege_fields,
        FieldID &bad_field, bool skip_privilege) const
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(our_req.handle_type == SINGULAR); // better be singular
#endif
      // Check to see if we found the requirement in the parent
      if (our_req.region == req.parent)
      {
        if ((req.handle_type == SINGULAR) || 
            (req.handle_type == REG_PROJECTION))
        {
          std::vector<ColorPoint> path;
          if (!runtime->forest->compute_index_path(req.parent.index_space,
                                            req.region.index_space, path))
            return ERROR_BAD_REGION_PATH;
        }
        else
        {
          std::vector<ColorPoint> path;
          if (!runtime->forest->compute_partition_path(req.parent.index_space,
                                        req.partition.index_partition, path))
            return ERROR_BAD_PARTITION_PATH;
        }
        // Now check that the types are subset of the fields
        // Note we can use the parent since all the regions/partitions
        // in the same region tree have the same field space
        for (std::set<FieldID>::const_iterator fit = 
              privilege_fields.begin(); fit != 
              privilege_fields.end(); )
        {
          if (our_req.privilege_fields.find(*fit) != 
              our_req.privilege_fields.end())
          {
            // Only need to do this check if there were overlapping fields
            if (!skip_privilege && (req.privilege & (~(our_req.privilege))))
            {
              // Handle the special case where the parent has WRITE_DISCARD
              // privilege and the sub-task wants any other kind of privilege.  
              // This case is ok because the parent could write something
              // and then hand it off to the child.
              if (our_req.privilege != WRITE_DISCARD)
              {
                if ((req.handle_type == SINGULAR) || 
                    (req.handle_type == REG_PROJECTION))
                  return ERROR_BAD_REGION_PRIVILEGES;
                else
                  return ERROR_BAD_PARTITION_PRIVILEGES;
              }
            }
            privilege_fields.erase(fit++);
          }
          else
            ++fit;
        }
      }

      if (!privilege_fields.empty()) return ERROR_BAD_PARENT_REGION;
        // If we make it here then we are good
      return NO_ERROR;
    }

    //--------------------------------------------------------------------------
    LogicalRegion TaskContext::find_logical_region(unsigned index)
    //--------------------------------------------------------------------------
    {
      if (index < regions.size())
        return regions[index].region;
      index -= regions.size();
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
      assert(index < created_requirements.size());
#endif
      return created_requirements[index].region;
    } 

    //--------------------------------------------------------------------------
    const std::vector<PhysicalRegion>& TaskContext::begin_task(void)
    //--------------------------------------------------------------------------
    {
      if (overhead_tracker != NULL)
        previous_profiling_time = Realm::Clock::current_time_in_nanoseconds();
      // Switch over the executing processor to the one
      // that has actually been assigned to run this task.
      executing_processor = Processor::get_executing_processor();
      if (Runtime::legion_spy_enabled)
        LegionSpy::log_task_processor(get_unique_id(), executing_processor.id);
#ifdef DEBUG_LEGION
      log_task.debug("Task %s (ID %lld) starting on processor " IDFMT "",
                    get_task_name(), get_unique_id(), executing_processor.id);
      assert(regions.size() == physical_regions.size());
#endif
      // Issue a utility task to decrement the number of outstanding
      // tasks now that this task has started running
      pending_done = owner_task->get_context()->decrement_pending(owner_task);
      return physical_regions;
    }

    //--------------------------------------------------------------------------
    void TaskContext::initialize_overhead_tracker(void)
    //--------------------------------------------------------------------------
    {
      // Make an overhead tracker
#ifdef DEBUG_LEGION
      assert(overhead_tracker == NULL);
#endif
      overhead_tracker = new 
        Mapping::ProfilingMeasurements::RuntimeOverhead();
    }

    //--------------------------------------------------------------------------
    void TaskContext::unmap_all_regions(void)
    //--------------------------------------------------------------------------
    {
      // Can't be holding the lock when we unmap in case we block
      std::vector<PhysicalRegion> unmap_regions;
      {
        AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
        for (std::vector<PhysicalRegion>::const_iterator it = 
              physical_regions.begin(); it != physical_regions.end(); it++)
        {
          if (it->impl->is_mapped())
            unmap_regions.push_back(*it);
        }
        // Also unmap any of our inline mapped physical regions
        for (LegionList<PhysicalRegion,TASK_INLINE_REGION_ALLOC>::
              tracked::const_iterator it = inline_regions.begin();
              it != inline_regions.end(); it++)
        {
          if (it->impl->is_mapped())
            unmap_regions.push_back(*it);
        }
      }
      // Perform the unmappings after we've released the lock
      for (std::vector<PhysicalRegion>::const_iterator it = 
            unmap_regions.begin(); it != unmap_regions.end(); it++)
      {
        if (it->impl->is_mapped())
          it->impl->unmap_region();
      }
    }

    //--------------------------------------------------------------------------
    void TaskContext::find_enclosing_local_fields(
           LegionDeque<LocalFieldInfo,TASK_LOCAL_FIELD_ALLOC>::tracked &infos)
    //--------------------------------------------------------------------------
    {
      // Ask the same for our parent context
      owner_task->get_context()->find_enclosing_local_fields(infos);
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      for (unsigned idx = 0; idx < local_fields.size(); idx++)
        infos.push_back(local_fields[idx]);
    } 

    /////////////////////////////////////////////////////////////
    // Inner Context 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InnerContext::InnerContext(Runtime *rt, TaskOp *owner,
                               const std::vector<RegionRequirement> &reqs,
                               const std::vector<unsigned> &parent_indexes,
                               const std::vector<bool> &virt_mapped)
      : TaskContext(rt, owner, reqs), 
        tree_context(rt->allocate_region_tree_context(this)),
        parent_req_indexes(parent_indexes), virtual_mapped(virt_mapped),
        total_children_count(0), total_close_count(0),
        outstanding_children_count(0), current_trace(NULL),
        valid_wait_event(false), outstanding_subtasks(0), pending_subtasks(0),
        pending_frames(0), currently_active_context(false), 
        current_fence(NULL), fence_gen(0) 
    //--------------------------------------------------------------------------
    {
      // Set some of the default values for a context
      context_configuration.max_window_size = 
        Runtime::initial_task_window_size;
      context_configuration.hysteresis_percentage = 
        Runtime::initial_task_window_hysteresis;
      context_configuration.max_outstanding_frames = 0;
      context_configuration.min_tasks_to_schedule = 
        Runtime::initial_tasks_to_schedule;
      context_configuration.min_frames_to_schedule = 0;
#ifdef DEBUG_LEGION
      assert(tree_context.exists());
      runtime->forest->check_context_state(tree_context);
#endif
    }

    //--------------------------------------------------------------------------
    InnerContext::InnerContext(const InnerContext &rhs)
      : TaskContext(NULL, NULL, rhs.regions), 
        parent_req_indexes(rhs.parent_req_indexes), 
        virtual_mapped(rhs.virtual_mapped)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    InnerContext::~InnerContext(void)
    //--------------------------------------------------------------------------
    {
      if (!remote_instances.empty())
      {
        UniqueID local_uid = get_unique_id();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize(local_uid);
        }
        for (std::map<AddressSpaceID,RemoteContext*>::const_iterator it = 
              remote_instances.begin(); it != remote_instances.end(); it++)
        {
          runtime->send_remote_context_free(it->first, rez);
        }
        remote_instances.clear();
      }
      for (std::map<TraceID,LegionTrace*>::const_iterator it = traces.begin();
            it != traces.end(); it++)
      {
        legion_delete(it->second);
      }
      traces.clear();
      // Clean up any locks and barriers that the user
      // asked us to destroy
      while (!context_locks.empty())
      {
        context_locks.back().destroy_reservation();
        context_locks.pop_back();
      }
      while (!context_barriers.empty())
      {
        Realm::Barrier bar = context_barriers.back();
        bar.destroy_barrier();
        context_barriers.pop_back();
      }
      local_fields.clear();
      if (valid_wait_event)
      {
        valid_wait_event = false;
        Runtime::trigger_event(window_wait);
      }
#ifdef DEBUG_LEGION
      assert(pending_top_views.empty());
      assert(outstanding_subtasks == 0);
      assert(pending_subtasks == 0);
      assert(pending_frames == 0);
#endif
    }

    //--------------------------------------------------------------------------
    InnerContext& InnerContext::operator=(const InnerContext &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    AddressSpaceID InnerContext::get_version_owner(RegionTreeNode *node,
                                                   AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock); 
      // See if we've already assigned it
      std::map<RegionTreeNode*,std::pair<AddressSpaceID,bool> >::iterator
        finder = region_tree_owners.find(node);
      // If we already assigned it then we are done
      if (finder != region_tree_owners.end())
      {
        // If it is remote only, see if it gets to stay that way
        if (finder->second.second && (source == runtime->address_space))
          finder->second.second = false; // no longer remote only
        return finder->second.first;
      }
      // Otherwise assign it to the source
      region_tree_owners[node] = 
        std::pair<AddressSpaceID,bool>(source, 
                                      (source != runtime->address_space));
      return source;
    }

    //--------------------------------------------------------------------------
    TaskContext* InnerContext::find_parent_context(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(owner_task != NULL);
#endif
      return owner_task->get_context();
    }

    //--------------------------------------------------------------------------
    InnerContext* InnerContext::find_parent_logical_context(unsigned index)
    //--------------------------------------------------------------------------
    {
      // If this is one of our original region requirements then
      // we can do the analysis in our original context
      const size_t owner_size = regions.size();
      if (index < owner_size)
        return this;
      // Otherwise we need to see if this going to be one of our
      // region requirements that returns privileges or not. If
      // it is then we do the analysis in the outermost context
      // otherwise we do it locally in our own context. We need
      // to hold the operation lock to look at this data structure.
      index -= owner_size;
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
#ifdef DEBUG_LEGION
      assert(index < returnable_privileges.size());
#endif
      if (returnable_privileges[index])
        return find_outermost_local_context();
      return this;
    }

    //--------------------------------------------------------------------------
    InnerContext* InnerContext::find_parent_physical_context(unsigned index)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(regions.size() == virtual_mapped.size());
      assert(regions.size() == parent_req_indexes.size());
#endif     
      if (index < virtual_mapped.size())
      {
        // See if it is virtual mapped
        if (virtual_mapped[index])
          return find_parent_context()->find_parent_physical_context(
                                            parent_req_indexes[index]);
        else // We mapped a physical instance so we're it
          return this;
      }
      else // We created it, put it in the top context
        return find_top_context();
    }

    //--------------------------------------------------------------------------
    void InnerContext::find_parent_version_info(unsigned index, unsigned depth,
                       const FieldMask &version_mask, VersionInfo &version_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(owner_task != NULL);
      assert(regions.size() == virtual_mapped.size()); 
#endif
      // If this isn't one of our original region requirements then 
      // we don't have any versions that the child won't discover itself
      // Same if the region was not virtually mapped
      if ((index >= virtual_mapped.size()) || !virtual_mapped[index])
        return;
      // We now need to clone any version info from the parent into the child
      const VersionInfo &parent_info = owner_task->get_version_info(index);  
      parent_info.clone_to_depth(depth, version_mask, version_info);
    }

    //--------------------------------------------------------------------------
    InnerContext* InnerContext::find_outermost_local_context(
                                                         InnerContext *previous)
    //--------------------------------------------------------------------------
    {
      TaskContext *parent = find_parent_context();
      if (parent != NULL)
        return parent->find_outermost_local_context(this);
#ifdef DEBUG_LEGION
      assert(previous != NULL);
#endif
      return previous;
    }

    //--------------------------------------------------------------------------
    InnerContext* InnerContext::find_top_context(void)
    //--------------------------------------------------------------------------
    {
      return find_parent_context()->find_top_context();
    }

    //--------------------------------------------------------------------------
    void InnerContext::pack_remote_context(Serializer &rez, 
                                           AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, PACK_REMOTE_CONTEXT_CALL);
#ifdef DEBUG_LEGION
      assert(owner_task != NULL);
#endif
      rez.serialize<bool>(false); // not the top-level context
      int depth = get_depth();
      rez.serialize(depth);
      // See if we need to pack up base task information
      owner_task->pack_external_task(rez, target);
#ifdef DEBUG_LEGION
      assert(regions.size() == parent_req_indexes.size());
#endif
      for (unsigned idx = 0; idx < regions.size(); idx++)
        rez.serialize(parent_req_indexes[idx]);
      // Pack up our virtual mapping information
      std::vector<unsigned> virtual_indexes;
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        if (virtual_mapped[idx])
          virtual_indexes.push_back(idx);
      }
      rez.serialize<size_t>(virtual_indexes.size());
      for (unsigned idx = 0; idx < virtual_indexes.size(); idx++)
        rez.serialize(virtual_indexes[idx]);
      // Pack up the version numbers only 
      const std::vector<VersionInfo> *version_infos = 
        owner_task->get_version_infos();
#ifdef DEBUG_LEGION
      assert(version_infos->size() == regions.size());
#endif
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        const VersionInfo &info = (*version_infos)[idx];
        // If we're virtually mapped, we need all the information
        if (virtual_mapped[idx])
          info.pack_version_info(rez);
        else
          info.pack_version_numbers(rez);
      }
      // Now pack up any local fields 
      LegionDeque<LocalFieldInfo,TASK_LOCAL_FIELD_ALLOC>::tracked locals = 
                                                                  local_fields;
      find_enclosing_local_fields(locals);
      size_t num_local = locals.size();
      rez.serialize(num_local);
      for (unsigned idx = 0; idx < locals.size(); idx++)
        rez.serialize(locals[idx]);
      rez.serialize(owner_task->get_task_completion());
      rez.serialize(find_parent_context()->get_context_uid());
    }

    //--------------------------------------------------------------------------
    void InnerContext::unpack_remote_context(Deserializer &derez,
                                           std::set<RtEvent> &preconditions)
    //--------------------------------------------------------------------------
    {
      assert(false); // should only be called for RemoteTask
    }

    //--------------------------------------------------------------------------
    void InnerContext::send_back_created_state(AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      if (created_requirements.empty())
        return;
#ifdef DEBUG_LEGION
      assert(created_requirements.size() == returnable_privileges.size());
#endif
      UniqueID target_context_uid = find_parent_context()->get_context_uid();
      for (unsigned idx = 0; idx < created_requirements.size(); idx++)
      {
        // Skip anything that doesn't have returnable privileges
        if (!returnable_privileges[idx])
          continue;
        const RegionRequirement &req = created_requirements[idx];
        // If it was deleted then we don't care
        if (was_created_requirement_deleted(req))
          continue;
        runtime->forest->send_back_logical_state(tree_context, 
                        target_context_uid, req, target);
      }
    }

    //--------------------------------------------------------------------------
    unsigned InnerContext::register_new_child_operation(Operation *op)
    //--------------------------------------------------------------------------
    {
      // If we are performing a trace mark that the child has a trace
      if (current_trace != NULL)
        op->set_trace(current_trace, !current_trace->is_fixed());
      unsigned result = total_children_count++;
      unsigned outstanding_count = 
        __sync_add_and_fetch(&outstanding_children_count,1);
      // Only need to check if we are not tracing by frames
      if ((context_configuration.min_frames_to_schedule == 0) && 
          (context_configuration.max_window_size > 0) && 
            (outstanding_count >= context_configuration.max_window_size))
      {
        // Try taking the lock first and see if we succeed
        RtEvent precondition = 
          Runtime::acquire_rt_reservation(context_lock, true/*exclusive*/);
        begin_task_wait(false/*from runtime*/);
        if (precondition.exists() && !precondition.has_triggered())
        {
          // Launch a window-wait task and then wait on the event 
          WindowWaitArgs args;
          args.parent_ctx = this;  
          RtEvent wait_done = 
            runtime->issue_runtime_meta_task(args, LG_RESOURCE_PRIORITY,
                                             owner_task, precondition);
          wait_done.wait();
        }
        else // we can do the wait inline
          perform_window_wait();
        end_task_wait();
      }
      if (Runtime::legion_spy_enabled)
        LegionSpy::log_child_operation_index(get_context_uid(), result, 
                                             op->get_unique_op_id()); 
      return result;
    }

    //--------------------------------------------------------------------------
    unsigned InnerContext::register_new_close_operation(CloseOp *op)
    //--------------------------------------------------------------------------
    {
      // For now we just bump our counter
      unsigned result = total_close_count++;
      if (Runtime::legion_spy_enabled)
        LegionSpy::log_close_operation_index(get_context_uid(), result, 
                                             op->get_unique_op_id());
      return result;
    }

    //--------------------------------------------------------------------------
    void InnerContext::perform_window_wait(void)
    //--------------------------------------------------------------------------
    {
      RtEvent wait_event;
      // We already hold our lock from the callsite above
      if (outstanding_children_count >= context_configuration.max_window_size)
      {
#ifdef DEBUG_LEGION
        assert(!valid_wait_event);
#endif
        window_wait = Runtime::create_rt_user_event();
        valid_wait_event = true;
        wait_event = window_wait;
      }
      // Release our lock now
      context_lock.release();
      if (wait_event.exists() && !wait_event.has_triggered())
        wait_event.wait();
    }

    //--------------------------------------------------------------------------
    void InnerContext::add_to_dependence_queue(Operation *op, bool has_lock,
                                               RtEvent op_precondition)
    //--------------------------------------------------------------------------
    {
      if (!has_lock)
      {
        RtEvent lock_acquire = Runtime::acquire_rt_reservation(context_lock,
                                true/*exclusive*/, last_registration);
        if (!lock_acquire.has_triggered())
        {
          AddToDepQueueArgs args;
          args.proxy_this = this;
          args.op = op;
          args.op_pre = op_precondition;
          last_registration = 
            runtime->issue_runtime_meta_task(args, LG_RESOURCE_PRIORITY,
                                             op, lock_acquire);
          return;
        }
      }
      // We have the lock
      if (op->is_tracking_parent())
      {
#ifdef DEBUG_LEGION
        assert(executing_children.find(op) == executing_children.end());
        assert(executed_children.find(op) == executed_children.end());
        assert(complete_children.find(op) == complete_children.end());
#endif       
        executing_children.insert(op);
      }
      // Issue the next dependence analysis task
      DeferredDependenceArgs args;
      args.op = op;
      // If we're ahead we give extra priority to the logical analysis
      // since it is on the critical path, but if not we give it the 
      // normal priority so that we can balance doing logical analysis
      // and actually mapping and running tasks
      if (op_precondition.exists())
      {
        RtEvent pre = Runtime::merge_events(op_precondition, 
                                            dependence_precondition);
        RtEvent next = runtime->issue_runtime_meta_task(args,
                                        currently_active_context ? 
                                          LG_THROUGHPUT_PRIORITY :
                                          LG_DEFERRED_THROUGHPUT_PRIORITY,
                                        op, pre);
        dependence_precondition = next;
      }
      else
      {
        RtEvent next = runtime->issue_runtime_meta_task(args,
                                        currently_active_context ? 
                                          LG_THROUGHPUT_PRIORITY :
                                          LG_DEFERRED_THROUGHPUT_PRIORITY,
                                        op, dependence_precondition);
        dependence_precondition = next;
      }
      // Now we can release the lock
      context_lock.release();
    }

    //--------------------------------------------------------------------------
    void InnerContext::register_child_executed(Operation *op)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
        std::set<Operation*>::iterator finder = executing_children.find(op);
#ifdef DEBUG_LEGION
        assert(finder != executing_children.end());
        assert(executed_children.find(op) == executed_children.end());
        assert(complete_children.find(op) == complete_children.end());
#endif
        executing_children.erase(finder);
        // Now put it in the list of executing operations
        // Note this doesn't change the number of active children
        // so there's no need to trigger any window waits
        //
        // Add some hysteresis here so that we have some runway for when
        // the paused task resumes it can run for a little while.
        executed_children.insert(op);
        int outstanding_count = 
          __sync_add_and_fetch(&outstanding_children_count,-1);
#ifdef DEBUG_LEGION
        assert(outstanding_count >= 0);
#endif
        if (valid_wait_event && (context_configuration.max_window_size > 0) &&
            (outstanding_count <=
             int(context_configuration.hysteresis_percentage * 
                 context_configuration.max_window_size / 100)))
        {
          to_trigger = window_wait;
          valid_wait_event = false;
        }
      }
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    void InnerContext::register_child_complete(Operation *op)
    //--------------------------------------------------------------------------
    {
      bool needs_trigger = false;
      {
        AutoLock ctx_lock(context_lock);
        std::set<Operation*>::iterator finder = executed_children.find(op);
#ifdef DEBUG_LEGION
        assert(finder != executed_children.end());
        assert(complete_children.find(op) == complete_children.end());
        assert(executing_children.find(op) == executing_children.end());
#endif
        executed_children.erase(finder);
        // Put it on the list of complete children to complete
        complete_children.insert(op);
        // See if we need to trigger the all children complete call
        if (task_executed && executing_children.empty() && 
            executed_children.empty() && !children_complete_invoked)
        {
          needs_trigger = true;
          children_complete_invoked = true;
        }
      }
      if (needs_trigger && (owner_task != NULL))
        owner_task->trigger_children_complete();
    }

    //--------------------------------------------------------------------------
    void InnerContext::register_child_commit(Operation *op)
    //--------------------------------------------------------------------------
    {
      bool needs_trigger = false;
      {
        AutoLock ctx_lock(context_lock);
        std::set<Operation*>::iterator finder = complete_children.find(op);
#ifdef DEBUG_LEGION
        assert(finder != complete_children.end());
        assert(executing_children.find(op) == executing_children.end());
        assert(executed_children.find(op) == executed_children.end());
#endif
        complete_children.erase(finder);
        // See if we need to trigger the all children commited call
        if (executing_children.empty() && 
            executed_children.empty() && complete_children.empty() &&
            !children_commit_invoked)
        {
          needs_trigger = true;
          children_commit_invoked = true;
        }
      }
      if (needs_trigger && (owner_task != NULL))
        owner_task->trigger_children_committed();
    }

    //--------------------------------------------------------------------------
    void InnerContext::unregister_child_operation(Operation *op)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
        // Remove it from everything and then see if we need to
        // trigger the window wait event
        executing_children.erase(op);
        executed_children.erase(op);
        complete_children.erase(op);
        int outstanding_count = 
          __sync_add_and_fetch(&outstanding_children_count,-1);
#ifdef DEBUG_LEGION
        assert(outstanding_count >= 0);
#endif
        if (valid_wait_event && (context_configuration.max_window_size > 0) &&
            (outstanding_count <=
             int(context_configuration.hysteresis_percentage * 
                 context_configuration.max_window_size / 100)))
        {
          to_trigger = window_wait;
          valid_wait_event = false;
        }
      }
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    void InnerContext::print_children(void)
    //--------------------------------------------------------------------------
    {
      // Don't both taking the lock since this is for debugging
      // and isn't actually called anywhere
      for (std::set<Operation*>::const_iterator it =
            executing_children.begin(); it != executing_children.end(); it++)
      {
        Operation *op = *it;
        printf("Executing Child %p\n",op);
      }
      for (std::set<Operation*>::const_iterator it =
            executed_children.begin(); it != executed_children.end(); it++)
      {
        Operation *op = *it;
        printf("Executed Child %p\n",op);
      }
      for (std::set<Operation*>::const_iterator it =
            complete_children.begin(); it != complete_children.end(); it++)
      {
        Operation *op = *it;
        printf("Complete Child %p\n",op);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::register_fence_dependence(Operation *op)
    //--------------------------------------------------------------------------
    {
      if (current_fence != NULL)
      {
#ifdef LEGION_SPY
        // Can't prune when doing legion spy
        op->register_dependence(current_fence, fence_gen);
        unsigned num_regions = op->get_region_count();
        if (num_regions > 0)
        {
          for (unsigned idx = 0; idx < num_regions; idx++)
          {
            LegionSpy::log_mapping_dependence(
                get_unique_op_id(), current_fence_uid, 0,
                op->get_unique_op_id(), idx, TRUE_DEPENDENCE);
          }
        }
        else
          LegionSpy::log_mapping_dependence(
              get_unique_op_id(), current_fence_uid, 0,
              op->get_unique_op_id(), 0, TRUE_DEPENDENCE);
#else
        // If we can prune it then go ahead and do so
        // No need to remove the mapping reference because 
        // the fence has already been committed
        if (op->register_dependence(current_fence, fence_gen))
          current_fence = NULL;
#endif
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::perform_fence_analysis(FenceOp *op)
    //--------------------------------------------------------------------------
    {
      RegionTreeContext ctx = get_context();
      // Do our internal regions first
      for (unsigned idx = 0; idx < regions.size(); idx++)
        runtime->forest->perform_fence_analysis(ctx, op, 
                                        regions[idx].region, true/*dominate*/);
      // Now see if we have any created regions
      // Track separately for the two possible contexts
      std::set<LogicalRegion> local_regions;
      std::set<LogicalRegion> outermost_regions;
      {
        AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
        if (created_requirements.empty())
          return;
        for (unsigned idx = 0; idx < created_requirements.size(); idx++)
        {
          const LogicalRegion &handle = created_requirements[idx].region;
          if (returnable_privileges[idx])
            outermost_regions.insert(handle);
          else
            local_regions.insert(handle);
        }
      }
      if (!local_regions.empty())
      {
        for (std::set<LogicalRegion>::const_iterator it = 
              local_regions.begin(); it != local_regions.end(); it++)
          runtime->forest->perform_fence_analysis(ctx,op,*it,true/*dominate*/);
      }
      if (!outermost_regions.empty())
      {
        // Need outermost context for these regions
        ctx = find_outermost_local_context()->get_context();
        for (std::set<LogicalRegion>::const_iterator it = 
              outermost_regions.begin(); it != outermost_regions.end(); it++)
          runtime->forest->perform_fence_analysis(ctx,op,*it,true/*dominate*/);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::update_current_fence(FenceOp *op)
    //--------------------------------------------------------------------------
    {
      if (current_fence != NULL)
        current_fence->remove_mapping_reference(fence_gen);
      current_fence = op;
      fence_gen = op->get_generation();
      current_fence->add_mapping_reference(fence_gen);
#ifdef LEGION_SPY
      current_fence_uid = op->get_unique_op_id();
#endif
    }

    //--------------------------------------------------------------------------
    void InnerContext::begin_trace(TraceID tid)
    //--------------------------------------------------------------------------
    {
      // No need to hold the lock here, this is only ever called
      // by the one thread that is running the task.
      if (current_trace != NULL)
      {
        log_task.error("Illegal nested trace with ID %d attempted in "
                       "task %s (ID %lld)", tid, get_task_name(),
                       get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_ILLEGAL_NESTED_TRACE);
      }
      std::map<TraceID,LegionTrace*>::const_iterator finder = traces.find(tid);
      if (finder == traces.end())
      {
        // Trace does not exist yet, so make one and record it
        current_trace = legion_new<LegionTrace>(tid, this);
        traces[tid] = current_trace;
      }
      else
      {
        // Issue the mapping fence first
        runtime->issue_mapping_fence(this);
        // Now mark that we are starting a trace
        current_trace = finder->second;
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::end_trace(TraceID tid)
    //--------------------------------------------------------------------------
    {
      if (current_trace == NULL)
      {
        log_task.error("Unmatched end trace for ID %d in task %s "
                       "(ID %lld)", tid, get_task_name(),
                       get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_UNMATCHED_END_TRACE);
      }
      if (current_trace->is_fixed())
      {
        // Already fixed, dump a complete trace op into the stream
        TraceCompleteOp *complete_op = runtime->get_available_trace_op(true);
        complete_op->initialize_complete(this);
        runtime->add_to_dependence_queue(get_executing_processor(),complete_op);
      }
      else
      {
        // Not fixed yet, dump a capture trace op into the stream
        TraceCaptureOp *capture_op = runtime->get_available_capture_op(true); 
        capture_op->initialize_capture(this);
        runtime->add_to_dependence_queue(get_executing_processor(), capture_op);
        // Mark that the current trace is now fixed
        current_trace->fix_trace();
      }
      // We no longer have a trace that we're executing 
      current_trace = NULL;
    }

    //--------------------------------------------------------------------------
    void InnerContext::issue_frame(FrameOp *frame, ApEvent frame_termination)
    //--------------------------------------------------------------------------
    {
      // This happens infrequently enough that we can just issue
      // a meta-task to see what we should do without holding the lock
      if (context_configuration.max_outstanding_frames > 0)
      {
        IssueFrameArgs args;
        args.parent_ctx = this;
        args.frame = frame;
        args.frame_termination = frame_termination;
        // We know that the issuing is done in order because we block after
        // we launch this meta-task which blocks the application task
        RtEvent wait_on = runtime->issue_runtime_meta_task(args,
                                      LG_LATENCY_PRIORITY, owner_task);
        wait_on.wait();
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::perform_frame_issue(FrameOp *frame,
                                         ApEvent frame_termination)
    //--------------------------------------------------------------------------
    {
      ApEvent wait_on, previous;
      {
        AutoLock ctx_lock(context_lock);
        const size_t current_frames = frame_events.size();
        if (current_frames > 0)
          previous = frame_events.back();
        if (current_frames > 
            (size_t)context_configuration.max_outstanding_frames)
          wait_on = frame_events[current_frames - 
                                 context_configuration.max_outstanding_frames];
        frame_events.push_back(frame_termination); 
      }
      frame->set_previous(previous);
      if (!wait_on.has_triggered())
        wait_on.wait();
    }

    //--------------------------------------------------------------------------
    void InnerContext::finish_frame(ApEvent frame_termination)
    //--------------------------------------------------------------------------
    {
      // Pull off all the frame events until we reach ours
      if (context_configuration.max_outstanding_frames > 0)
      {
        AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
        assert(frame_events.front() == frame_termination);
#endif
        frame_events.pop_front();
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::increment_outstanding(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert((context_configuration.min_tasks_to_schedule == 0) || 
             (context_configuration.min_frames_to_schedule == 0));
      assert((context_configuration.min_tasks_to_schedule > 0) || 
             (context_configuration.min_frames_to_schedule > 0));
#endif
      RtEvent wait_on;
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
        if (!currently_active_context && (outstanding_subtasks == 0) && 
            (((context_configuration.min_tasks_to_schedule > 0) && 
              (pending_subtasks < 
               context_configuration.min_tasks_to_schedule)) ||
             ((context_configuration.min_frames_to_schedule > 0) &&
              (pending_frames < 
               context_configuration.min_frames_to_schedule))))
        {
          wait_on = context_order_event;
          to_trigger = Runtime::create_rt_user_event();
          context_order_event = to_trigger;
          currently_active_context = true;
        }
        outstanding_subtasks++;
      }
      if (to_trigger.exists())
      {
        wait_on.wait();
        runtime->activate_context(this);
        Runtime::trigger_event(to_trigger);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::decrement_outstanding(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert((context_configuration.min_tasks_to_schedule == 0) || 
             (context_configuration.min_frames_to_schedule == 0));
      assert((context_configuration.min_tasks_to_schedule > 0) || 
             (context_configuration.min_frames_to_schedule > 0));
#endif
      RtEvent wait_on;
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
        assert(outstanding_subtasks > 0);
#endif
        outstanding_subtasks--;
        if (currently_active_context && (outstanding_subtasks == 0) && 
            (((context_configuration.min_tasks_to_schedule > 0) &&
              (pending_subtasks < 
               context_configuration.min_tasks_to_schedule)) ||
             ((context_configuration.min_frames_to_schedule > 0) &&
              (pending_frames < 
               context_configuration.min_frames_to_schedule))))
        {
          wait_on = context_order_event;
          to_trigger = Runtime::create_rt_user_event();
          context_order_event = to_trigger;
          currently_active_context = false;
        }
      }
      if (to_trigger.exists())
      {
        wait_on.wait();
        runtime->deactivate_context(this);
        Runtime::trigger_event(to_trigger);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::increment_pending(void)
    //--------------------------------------------------------------------------
    {
      // Don't need to do this if we are scheduling based on mapped frames
      if (context_configuration.min_tasks_to_schedule == 0)
        return;
      RtEvent wait_on;
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
        pending_subtasks++;
        if (currently_active_context && (outstanding_subtasks > 0) &&
            (pending_subtasks == context_configuration.min_tasks_to_schedule))
        {
          wait_on = context_order_event;
          to_trigger = Runtime::create_rt_user_event();
          context_order_event = to_trigger;
          currently_active_context = false;
        }
      }
      if (to_trigger.exists())
      {
        wait_on.wait();
        runtime->deactivate_context(this);
        Runtime::trigger_event(to_trigger);
      }
    }

    //--------------------------------------------------------------------------
    RtEvent InnerContext::decrement_pending(TaskOp *child) const
    //--------------------------------------------------------------------------
    {
      // Don't need to do this if we are scheduled by frames
      if (context_configuration.min_tasks_to_schedule == 0)
        return RtEvent::NO_RT_EVENT;
      // This may involve waiting, so always issue it as a meta-task 
      DecrementArgs decrement_args;
      decrement_args.parent_ctx = const_cast<InnerContext*>(this);
      RtEvent precondition = 
        Runtime::acquire_rt_reservation(context_lock, true/*exclusive*/);
      return runtime->issue_runtime_meta_task(decrement_args, 
                  LG_RESOURCE_PRIORITY, child, precondition);
    }

    //--------------------------------------------------------------------------
    void InnerContext::decrement_pending(void)
    //--------------------------------------------------------------------------
    {
      RtEvent wait_on;
      RtUserEvent to_trigger;
      // We already hold the lock from the dispatch site (see above)
#ifdef DEBUG_LEGION
      assert(pending_subtasks > 0);
#endif
      pending_subtasks--;
      if (!currently_active_context && (outstanding_subtasks > 0) &&
          (pending_subtasks < context_configuration.min_tasks_to_schedule))
      {
        wait_on = context_order_event;
        to_trigger = Runtime::create_rt_user_event();
        context_order_event = to_trigger;
        currently_active_context = true;
      }
      // Release the lock before doing the trigger or the wait
      context_lock.release();
      // Do anything that we need to do
      if (to_trigger.exists())
      {
        wait_on.wait();
        runtime->activate_context(this);
        Runtime::trigger_event(to_trigger);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::increment_frame(void)
    //--------------------------------------------------------------------------
    {
      // Don't need to do this if we are scheduling based on mapped tasks
      if (context_configuration.min_frames_to_schedule == 0)
        return;
      RtEvent wait_on;
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
        pending_frames++;
        if (currently_active_context && (outstanding_subtasks > 0) &&
            (pending_frames == context_configuration.min_frames_to_schedule))
        {
          wait_on = context_order_event;
          to_trigger = Runtime::create_rt_user_event();
          context_order_event = to_trigger;
          currently_active_context = false;
        }
      }
      if (to_trigger.exists())
      {
        wait_on.wait();
        runtime->deactivate_context(this);
        Runtime::trigger_event(to_trigger);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::decrement_frame(void)
    //--------------------------------------------------------------------------
    {
      // Don't need to do this if we are scheduling based on mapped tasks
      if (context_configuration.min_frames_to_schedule == 0)
        return;
      RtEvent wait_on;
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
        assert(pending_frames > 0);
#endif
        pending_frames--;
        if (!currently_active_context && (outstanding_subtasks > 0) &&
            (pending_frames < context_configuration.min_frames_to_schedule))
        {
          wait_on = context_order_event;
          to_trigger = Runtime::create_rt_user_event();
          context_order_event = to_trigger;
          currently_active_context = true;
        }
      }
      if (to_trigger.exists())
      {
        wait_on.wait();
        runtime->activate_context(this);
        Runtime::trigger_event(to_trigger);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::add_acquisition(AcquireOp *op,
                                       const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      if (!runtime->forest->add_acquisition(coherence_restrictions, op, req))
      {
        // We faiiled to acquire, report the error
        log_run.error("Illegal acquire operation (ID %lld) performed in "
                      "task %s (ID %lld). Acquire was performed on a non-"
                      "restricted region.", op->get_unique_op_id(),
                      get_task_name(), get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_UNRESTRICTED_ACQUIRE);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::remove_acquisition(ReleaseOp *op, 
                                          const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      if (!runtime->forest->remove_acquisition(coherence_restrictions, op, req))
      {
        // We failed to release, report the error
        log_run.error("Illegal release operation (ID %lld) performed in "
                      "task %s (ID %lld). Release was performed on a region "
                      "that had not previously been acquired.",
                      op->get_unique_op_id(), get_task_name(), 
                      get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_UNACQUIRED_RELEASE);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::add_restriction(AttachOp *op, InstanceManager *inst,
                                       const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      runtime->forest->add_restriction(coherence_restrictions, op, inst, req);
    }

    //--------------------------------------------------------------------------
    void InnerContext::remove_restriction(DetachOp *op, 
                                          const RegionRequirement &req)
    //--------------------------------------------------------------------------
    {
      if (!runtime->forest->remove_restriction(coherence_restrictions, op, req))
      {
        // We failed to remove the restriction
        log_run.error("Illegal detach operation (ID %lld) performed in "
                      "task %s (ID %lld). Detach was performed on an region "
                      "that had not previously been attached.",
                      op->get_unique_op_id(), get_task_name(),
                      get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_UNATTACHED_DETACH);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::release_restrictions(void)
    //--------------------------------------------------------------------------
    {
      for (std::list<Restriction*>::const_iterator it = 
            coherence_restrictions.begin(); it != 
            coherence_restrictions.end(); it++)
        delete (*it);
      coherence_restrictions.clear();
    }

    //--------------------------------------------------------------------------
    void InnerContext::perform_restricted_analysis(const RegionRequirement &req,
                                                   RestrictInfo &restrict_info)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!coherence_restrictions.empty());
#endif
      runtime->forest->perform_restricted_analysis(
                                coherence_restrictions, req, restrict_info);
    }

    //--------------------------------------------------------------------------
    void InnerContext::configure_context(MapperManager *mapper)
    //--------------------------------------------------------------------------
    {
      mapper->invoke_configure_context(owner_task, &context_configuration);
      // Do a little bit of checking on the output.  Make
      // sure that we only set one of the two cases so we
      // are counting by frames or by outstanding tasks.
      if ((context_configuration.min_tasks_to_schedule == 0) && 
          (context_configuration.min_frames_to_schedule == 0))
      {
        log_run.error("Invalid mapper output from call 'configure_context' "
                      "on mapper %s. One of 'min_tasks_to_schedule' and "
                      "'min_frames_to_schedule' must be non-zero for task "
                      "%s (ID %lld)", mapper->get_mapper_name(),
                      get_task_name(), get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_CONTEXT_CONFIGURATION);
      }
      // If we're counting by frames set min_tasks_to_schedule to zero
      if (context_configuration.min_frames_to_schedule > 0)
        context_configuration.min_tasks_to_schedule = 0;
      // otherwise we know min_frames_to_schedule is zero
    }

    //--------------------------------------------------------------------------
    void InnerContext::initialize_region_tree_contexts(
                      const std::vector<RegionRequirement> &clone_requirements,
                      const std::vector<ApUserEvent> &unmap_events,
                      std::set<ApEvent> &preconditions,
                      std::set<RtEvent> &applied_events)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, INITIALIZE_REGION_TREE_CONTEXTS_CALL);
      // Save to cast to single task here because this will never
      // happen during inlining of index space tasks
#ifdef DEBUG_LEGION
      assert(owner_task != NULL);
      SingleTask *single_task = dynamic_cast<SingleTask*>(owner_task);
      assert(single_task != NULL);
#else
      SingleTask *single_task = static_cast<SingleTask*>(owner_task); 
#endif
      const LegionDeque<InstanceSet,TASK_INSTANCE_REGION_ALLOC>::tracked&
        physical_instances = single_task->get_physical_instances();
      const std::vector<bool> &no_access_regions = 
        single_task->get_no_access_regions();
#ifdef DEBUG_LEGION
      assert(regions.size() == physical_instances.size());
      assert(regions.size() == virtual_mapped.size());
      assert(regions.size() == no_access_regions.size());
#endif
      // Initialize all of the logical contexts no matter what
      //
      // For all of the physical contexts that were mapped, initialize them
      // with a specified reference to the current instance, otherwise
      // they were a virtual reference and we can ignore it.
      std::map<PhysicalManager*,InstanceView*> top_views;
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
#ifdef DEBUG_LEGION
        // this better be true for single tasks
        assert(regions[idx].handle_type == SINGULAR);
#endif
        // If this is a NO_ACCESS or had no privilege fields we can skip this
        if (no_access_regions[idx])
          continue;
        // Only need to initialize the context if this is
        // not a leaf and it wasn't virtual mapped
        if (!virtual_mapped[idx])
        {
          runtime->forest->initialize_current_context(tree_context,
              clone_requirements[idx], physical_instances[idx],
              unmap_events[idx], this, idx, top_views, applied_events);
#ifdef DEBUG_LEGION
          assert(!physical_instances[idx].empty());
#endif
          // Always make reduce-only privileges restricted so that
          // we always flush data back, this will prevent us from 
          // needing a post close op later
          if (IS_REDUCE(regions[idx]))
            coherence_restrictions.push_back(
                runtime->forest->create_coherence_restriction(regions[idx],
                                                  physical_instances[idx]));
          // If we need to add restricted coherence, do that now
          // Not we only need to do this for non-virtually mapped task
          else if ((regions[idx].prop == SIMULTANEOUS) && 
                   ((regions[idx].privilege == READ_ONLY) ||
                    (regions[idx].privilege == READ_WRITE) ||
                    (regions[idx].privilege == WRITE_DISCARD)))
            coherence_restrictions.push_back(
                runtime->forest->create_coherence_restriction(regions[idx],
                                                  physical_instances[idx]));
        }
        else
        {
          runtime->forest->initialize_virtual_context(tree_context,
                                          clone_requirements[idx]);
        }
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::invalidate_region_tree_contexts(void)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, INVALIDATE_REGION_TREE_CONTEXTS_CALL);
      // Invalidate all our region contexts
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        runtime->forest->invalidate_current_context(tree_context,
                                                    false/*users only*/,
                                                    regions[idx].region);
        if (!virtual_mapped[idx])
          runtime->forest->invalidate_versions(tree_context, 
                                               regions[idx].region);
      }
      if (!created_requirements.empty())
      {
        TaskContext *outermost = find_outermost_local_context();
        RegionTreeContext outermost_ctx = outermost->get_context();
        const bool is_outermost = (outermost == this);
        for (unsigned idx = 0; idx < created_requirements.size(); idx++)
        {
          // See if we're a returnable privilege or not
          if (returnable_privileges[idx])
          {
            // If we're the outermost context or the requirement was
            // deleted, then we can invalidate everything
            // Otherwiswe we only invalidate the users
            const bool users_only = !is_outermost && 
              !was_created_requirement_deleted(created_requirements[idx]);
            runtime->forest->invalidate_current_context(outermost_ctx,
                        users_only, created_requirements[idx].region);
          }
          else // Not returning so invalidate the full thing 
            runtime->forest->invalidate_current_context(tree_context,
                false/*users only*/, created_requirements[idx].region);
        }
      }
      // Clean up our instance top views
      if (!instance_top_views.empty())
      {
        for (std::map<PhysicalManager*,InstanceView*>::const_iterator it = 
              instance_top_views.begin(); it != instance_top_views.end(); it++)
        {
          it->first->unregister_active_context(this);
          if (it->second->remove_base_resource_ref(CONTEXT_REF))
            LogicalView::delete_logical_view(it->second);
        }
        instance_top_views.clear();
      }
      // Before freeing our context, see if there are any version
      // state managers we need to reset
      if (!region_tree_owners.empty())
      {
        for (std::map<RegionTreeNode*,
                      std::pair<AddressSpaceID,bool> >::const_iterator it =
              region_tree_owners.begin(); it != region_tree_owners.end(); it++)
        {
          // If this is a remote only then we don't need to invalidate it
          if (!it->second.second)
            it->first->invalidate_version_state(tree_context.get_id()); 
        }
        region_tree_owners.clear();
      }
      // Now we can free our region tree context
      runtime->free_region_tree_context(tree_context, this);
    }

    //--------------------------------------------------------------------------
    InstanceView* InnerContext::create_instance_top_view(
                        PhysicalManager *manager, AddressSpaceID request_source,
                        RtEvent *ready_event/*=NULL*/)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, CREATE_INSTANCE_TOP_VIEW_CALL);
      // First check to see if we are the owner node for this manager
      // if not we have to send the message there since the context
      // on that node is actually the point of serialization
      if (!manager->is_owner())
      {
        InstanceView *volatile result = NULL;
        RtUserEvent wait_on = Runtime::create_rt_user_event();
        Serializer rez;
        {
          RezCheck z(rez);
          rez.serialize<UniqueID>(get_context_uid());
          rez.serialize(manager->did);
          rez.serialize<InstanceView**>(const_cast<InstanceView**>(&result));
          rez.serialize(wait_on); 
        }
        runtime->send_create_top_view_request(manager->owner_space, rez);
        wait_on.wait();
#ifdef DEBUG_LEGION
        assert(result != NULL); // when we wake up we should have the result
#endif
        return result;
      }
      // Check to see if we already have the 
      // instance, if we do, return it, otherwise make it and save it
      RtEvent wait_on;
      {
        AutoLock ctx_lock(context_lock);
        std::map<PhysicalManager*,InstanceView*>::const_iterator finder = 
          instance_top_views.find(manager);
        if (finder != instance_top_views.end())
          // We've already got the view, so we are done
          return finder->second;
        // See if someone else is already making it
        std::map<PhysicalManager*,RtUserEvent>::iterator pending_finder =
          pending_top_views.find(manager);
        if (pending_finder == pending_top_views.end())
          // mark that we are making it
          pending_top_views[manager] = RtUserEvent::NO_RT_USER_EVENT;
        else
        {
          // See if we are the first one to follow
          if (!pending_finder->second.exists())
            pending_finder->second = Runtime::create_rt_user_event();
          wait_on = pending_finder->second;
        }
      }
      if (wait_on.exists())
      {
        // Someone else is making it so we just have to wait for it
        wait_on.wait();
        // Retake the lock and read out the result
        AutoLock ctx_lock(context_lock, 1, false/*exclusive*/);
        std::map<PhysicalManager*,InstanceView*>::const_iterator finder = 
            instance_top_views.find(manager);
#ifdef DEBUG_LEGION
        assert(finder != instance_top_views.end());
#endif
        return finder->second;
      }
      InstanceView *result = 
        manager->create_instance_top_view(this, request_source);
      result->add_base_resource_ref(CONTEXT_REF);
      // Record the result and trigger any user event to signal that the
      // view is ready
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
        assert(instance_top_views.find(manager) == 
                instance_top_views.end());
#endif
        instance_top_views[manager] = result;
        std::map<PhysicalManager*,RtUserEvent>::iterator pending_finder =
          pending_top_views.find(manager);
#ifdef DEBUG_LEGION
        assert(pending_finder != pending_top_views.end());
#endif
        to_trigger = pending_finder->second;
        pending_top_views.erase(pending_finder);
      }
      if (to_trigger.exists())
        Runtime::trigger_event(to_trigger);
      return result;
    }

    //--------------------------------------------------------------------------
    void InnerContext::notify_instance_deletion(PhysicalManager *deleted)
    //--------------------------------------------------------------------------
    {
      InstanceView *removed = NULL;
      {
        AutoLock ctx_lock(context_lock);
        std::map<PhysicalManager*,InstanceView*>::iterator finder =  
          instance_top_views.find(deleted);
#ifdef DEBUG_LEGION
        assert(finder != instance_top_views.end());
#endif
        removed = finder->second;
        instance_top_views.erase(finder);
      }
      if (removed->remove_base_resource_ref(CONTEXT_REF))
        LogicalView::delete_logical_view(removed);
    }

    //--------------------------------------------------------------------------
    /*static*/ void InnerContext::handle_create_top_view_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      UniqueID context_uid;
      derez.deserialize(context_uid);
      DistributedID manager_did;
      derez.deserialize(manager_did);
      InstanceView **target;
      derez.deserialize(target);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      // Get the context first
      InnerContext *context = runtime->find_context(context_uid);
      // Find the manager too, we know we are local so it should already
      // be registered in the set of distributed IDs
      DistributedCollectable *dc = 
        runtime->find_distributed_collectable(manager_did);
#ifdef DEBUG_LEGION
      PhysicalManager *manager = dynamic_cast<PhysicalManager*>(dc);
      assert(manager != NULL);
#else
      PhysicalManager *manager = static_cast<PhysicalManager*>(dc);
#endif
      // Nasty deadlock case: if the request came from a different node
      // we have to defer this because we are in the view virtual channel
      // and we might invoke the update virtual channel, but we already
      // know it's possible for the update channel to block waiting on
      // the view virtual channel (paging views), so to avoid the cycle
      // we have to launch a meta-task and record when it is done
      RemoteCreateViewArgs args;
      args.proxy_this = context;
      args.manager = manager;
      args.target = target;
      args.to_trigger = to_trigger;
      args.source = source;
      runtime->issue_runtime_meta_task(args, LG_LATENCY_PRIORITY, 
                                       context->get_owner_task());
    }

    //--------------------------------------------------------------------------
    /*static*/ void InnerContext::handle_remote_view_creation(const void *args)
    //--------------------------------------------------------------------------
    {
      const RemoteCreateViewArgs *rargs = (const RemoteCreateViewArgs*)args;
      
      InstanceView *result = rargs->proxy_this->create_instance_top_view(
                                                 rargs->manager, rargs->source);
      // Now we can send the response
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(result->did);
        rez.serialize(rargs->target);
        rez.serialize(rargs->to_trigger);
      }
      rargs->proxy_this->runtime->send_create_top_view_response(rargs->source, 
                                                                rez);
    }

    //--------------------------------------------------------------------------
    /*static*/ void InnerContext::handle_create_top_view_response(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      DerezCheck z(derez);
      DistributedID result_did;
      derez.deserialize(result_did);
      InstanceView **target;
      derez.deserialize(target);
      RtUserEvent to_trigger;
      derez.deserialize(to_trigger);
      RtEvent ready;
      LogicalView *view = 
        runtime->find_or_request_logical_view(result_did, ready);
      // Have to static cast since it might not be ready
      *target = static_cast<InstanceView*>(view);
      if (ready.exists())
        Runtime::trigger_event(to_trigger, ready);
      else
        Runtime::trigger_event(to_trigger);
    }

#ifdef LEGION_SPY
    //--------------------------------------------------------------------------
    RtEvent InnerContext::update_previous_mapped_event(RtEvent next)
    //--------------------------------------------------------------------------
    {
      RtEvent result = previous_mapped_event;
      previous_mapped_event = next;
      return result;
    }
#endif

    //--------------------------------------------------------------------------
    bool InnerContext::attempt_children_complete(void)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      if (task_executed && executing_children.empty() && 
          executed_children.empty() && !children_complete_invoked)
      {
        children_complete_invoked = true;
        return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    bool InnerContext::attempt_children_commit(void)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      if (task_executed && executing_children.empty() && 
          executed_children.empty() && complete_children.empty() && 
          !children_commit_invoked)
      {
        children_commit_invoked = true;
        return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    void InnerContext::end_task(const void *res, size_t res_size, bool owned)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(owner_task != NULL);
      runtime->decrement_total_outstanding_tasks(owner_task->task_id, 
                                                 false/*meta*/);
#else
      runtime->decrement_total_outstanding_tasks();
#endif
      if (overhead_tracker != NULL)
      {
        const long long current = Realm::Clock::current_time_in_nanoseconds();
        const long long diff = current - previous_profiling_time;
        overhead_tracker->application_time += diff;
      }
      // Quick check to make sure the user didn't forget to end a trace
      if (current_trace != NULL)
      {
        log_task.error("Task %s (UID %lld) failed to end trace before exiting!",
                        get_task_name(), get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INCOMPLETE_TRACE);
      }
      // We can unmap all the inline regions here, we'll have to wait to
      // do the physical_regions until post_end_task when we can take
      // the operation lock
      for (std::list<PhysicalRegion>::const_iterator it = 
            inline_regions.begin(); it != inline_regions.end(); it++)
      {
        if (it->impl->is_mapped())
          it->impl->unmap_region();
      }
      inline_regions.clear();
      const bool is_leaf = is_leaf_context();
      // Safe to cast to a single task here because this will never
      // be called while inlining an index space task
#ifdef DEBUG_LEGION
      SingleTask *single_task = dynamic_cast<SingleTask*>(owner_task);
      assert(single_task != NULL);
#else
      SingleTask *single_task = static_cast<SingleTask*>(owner_task);
#endif
      if (!is_leaf || single_task->has_virtual_instances())
      {
        const LegionDeque<InstanceSet,TASK_INSTANCE_REGION_ALLOC>::tracked&
          physical_instances = single_task->get_physical_instances();
        // Note that this loop doesn't handle create regions
        // we deal with that case below
        for (unsigned idx = 0; idx < physical_instances.size(); idx++)
        {
          // We also don't need to close up read-only instances
          // or reduction-only instances (because they are restricted)
          // so all changes have already been propagated
          if (!IS_WRITE(regions[idx]))
            continue;
          if (!virtual_mapped[idx])
          {
            if (!is_leaf)
            {
#ifdef DEBUG_LEGION
              assert(!physical_instances[idx].empty());
#endif
              PostCloseOp *close_op = 
                runtime->get_available_post_close_op(true);
              close_op->initialize(this, idx);
              runtime->add_to_dependence_queue(executing_processor, close_op);
            }
          }
          else
          {
            // Make a virtual close op to close up the instance
            VirtualCloseOp *close_op = 
              runtime->get_available_virtual_close_op(true);
            close_op->initialize(this, idx, regions[idx]);
            runtime->add_to_dependence_queue(executing_processor, close_op);
          }
        }
      } 
      // See if we want to move the rest of this computation onto
      // the utility processor. We also need to be sure that we have 
      // registered all of our operations before we can do the post end task
      if (runtime->has_explicit_utility_procs || 
          !last_registration.has_triggered())
      {
        PostEndArgs post_end_args;
        post_end_args.proxy_this = this;
        post_end_args.result_size = res_size;
        // If it is not owned make a copy
        if (!owned)
        {
          post_end_args.result = malloc(res_size);
          memcpy(post_end_args.result, res, res_size);
        }
        else
          post_end_args.result = const_cast<void*>(res);
        // Give these high priority too since they are cleaning up 
        // and will allow other tasks to run
        runtime->issue_runtime_meta_task(post_end_args,
           LG_LATENCY_PRIORITY, owner_task, last_registration);
      }
      else
        post_end_task(res, res_size, owned);
    }

    //--------------------------------------------------------------------------
    void InnerContext::post_end_task(const void *res,size_t res_size,bool owned)
    //--------------------------------------------------------------------------
    {
      // Safe to cast to a single task here because this will never
      // be called while inlining an index space task
#ifdef DEBUG_LEGION
      SingleTask *single_task = dynamic_cast<SingleTask*>(owner_task);
      assert(single_task != NULL);
#else
      SingleTask *single_task = static_cast<SingleTask*>(owner_task);
#endif
      // Handle the future result
      single_task->handle_future(res, res_size, owned);
      // If we weren't a leaf task, compute the conditions for being mapped
      // which is that all of our children are now mapped
      // Also test for whether we need to trigger any of our child
      // complete or committed operations before marking that we
      // are done executing
      bool need_complete = false;
      bool need_commit = false;
      std::vector<PhysicalRegion> unmap_regions;
      if (is_leaf_context())
      {
        std::set<RtEvent> preconditions;
        {
          AutoLock ctx_lock(context_lock);
          // Only need to do this for executing and executed children
          // We know that any complete children are done
          for (std::set<Operation*>::const_iterator it = 
                executing_children.begin(); it != 
                executing_children.end(); it++)
          {
            preconditions.insert((*it)->get_mapped_event());
          }
          for (std::set<Operation*>::const_iterator it = 
                executed_children.begin(); it != executed_children.end(); it++)
          {
            preconditions.insert((*it)->get_mapped_event());
          }
#ifdef DEBUG_LEGION
          assert(!task_executed);
#endif
          // Now that we know the last registration has taken place we
          // can mark that we are done executing
          task_executed = true;
          if (executing_children.empty() && executed_children.empty())
          {
            if (!children_complete_invoked)
            {
              need_complete = true;
              children_complete_invoked = true;
            }
            if (complete_children.empty() && 
                !children_commit_invoked)
            {
              need_commit = true;
              children_commit_invoked = true;
            }
          }
          // Finally unmap any of our mapped physical instances
#ifdef DEBUG_LEGION
          assert((regions.size() + 
                    created_requirements.size()) == physical_regions.size());
#endif
          for (std::vector<PhysicalRegion>::const_iterator it = 
                physical_regions.begin(); it != physical_regions.end(); it++)
          {
            if (it->impl->is_mapped())
              unmap_regions.push_back(*it);
          }
        }
        if (!preconditions.empty())
          single_task->handle_post_mapped(Runtime::merge_events(preconditions));
        else
          single_task->handle_post_mapped();
      }
      else
      {
        // Handle the leaf task case
        AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
        assert(!task_executed);
#endif
        // Now that we know the last registration has taken place we
        // can mark that we are done executing
        task_executed = true;
        if (executing_children.empty() && executed_children.empty())
        {
          if (!children_complete_invoked)
          {
            need_complete = true;
            children_complete_invoked = true;
          }
          if (complete_children.empty() && 
              !children_commit_invoked)
          {
            need_commit = true;
            children_commit_invoked = true;
          }
        }
        // Finally unmap any physical regions that we mapped
#ifdef DEBUG_LEGION
        assert((regions.size() + 
                  created_requirements.size()) == physical_regions.size());
#endif
        for (std::vector<PhysicalRegion>::const_iterator it = 
              physical_regions.begin(); it != physical_regions.end(); it++)
        {
          if (it->impl->is_mapped())
            unmap_regions.push_back(*it);
        }
      }
      // Do the unmappings while not holding the lock in case we block
      if (!unmap_regions.empty())
      {
        for (std::vector<PhysicalRegion>::const_iterator it = 
              unmap_regions.begin(); it != unmap_regions.end(); it++)
          it->impl->unmap_region();
      }
      // Mark that we are done executing this operation
      // We're not actually done until we have registered our pending
      // decrement of our parent task and recorded any profiling
      if (!pending_done.has_triggered())
        owner_task->complete_execution(pending_done);
      else
        owner_task->complete_execution();
      if (need_complete)
        owner_task->trigger_children_complete();
      if (need_commit)
        owner_task->trigger_children_committed();
    }

    //--------------------------------------------------------------------------
    void InnerContext::send_remote_context(AddressSpaceID remote_instance,
                                           RemoteContext *remote_ctx)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(remote_instance != runtime->address_space);
#endif
      Serializer rez;
      {
        RezCheck z(rez);
        rez.serialize(remote_ctx);
        pack_remote_context(rez, remote_instance);
      }
      runtime->send_remote_context_response(remote_instance, rez);
      AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
      assert(remote_instances.find(remote_instance) == remote_instances.end());
#endif
      remote_instances[remote_instance] = remote_ctx;
    }

    //--------------------------------------------------------------------------
    /*static*/ void InnerContext::handle_version_owner_request(
                   Deserializer &derez, Runtime *runtime, AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      UniqueID context_uid;
      derez.deserialize(context_uid);
      InnerContext *local_ctx = runtime->find_context(context_uid);
      InnerContext *remote_ctx;
      derez.deserialize(remote_ctx);
      bool is_region;
      derez.deserialize(is_region);

      Serializer rez;
      rez.serialize(remote_ctx);
      if (is_region)
      {
        LogicalRegion handle;
        derez.deserialize(handle);
        RegionTreeNode *node = runtime->forest->get_node(handle);

        AddressSpaceID result = local_ctx->get_version_owner(node, source);
        rez.serialize(result);
        rez.serialize<bool>(true);
        rez.serialize(handle);
      }
      else
      {
        LogicalPartition handle;
        derez.deserialize(handle);
        RegionTreeNode *node = runtime->forest->get_node(handle);

        AddressSpaceID result = local_ctx->get_version_owner(node, source);
        rez.serialize(result);
        rez.serialize<bool>(false);
        rez.serialize(handle);
      }
      runtime->send_version_owner_response(source, rez);
    }

    //--------------------------------------------------------------------------
    void InnerContext::process_version_owner_response(RegionTreeNode *node,
                                                      AddressSpaceID result)
    //--------------------------------------------------------------------------
    {
      RtUserEvent to_trigger;
      {
        AutoLock ctx_lock(context_lock);
#ifdef DEBUG_LEGION
        assert(region_tree_owners.find(node) == region_tree_owners.end());
#endif
        region_tree_owners[node] = 
          std::pair<AddressSpaceID,bool>(result, false/*remote only*/); 
        // Find the event to trigger
        std::map<RegionTreeNode*,RtUserEvent>::iterator finder = 
          pending_version_owner_requests.find(node);
#ifdef DEBUG_LEGION
        assert(finder != pending_version_owner_requests.end());
#endif
        to_trigger = finder->second;
        pending_version_owner_requests.erase(finder);
      }
      Runtime::trigger_event(to_trigger);
    }

    //--------------------------------------------------------------------------
    /*static*/ void InnerContext::handle_version_owner_response(
                                          Deserializer &derez, Runtime *runtime)
    //--------------------------------------------------------------------------
    {
      InnerContext *ctx;
      derez.deserialize(ctx);
      AddressSpaceID result;
      derez.deserialize(result);
      bool is_region;
      derez.deserialize(is_region);
      if (is_region)
      {
        LogicalRegion handle;
        derez.deserialize(handle);
        RegionTreeNode *node = runtime->forest->get_node(handle);
        ctx->process_version_owner_response(node, result);
      }
      else
      {
        LogicalPartition handle;
        derez.deserialize(handle);
        RegionTreeNode *node = runtime->forest->get_node(handle);
        ctx->process_version_owner_response(node, result);
      }
    }

    //--------------------------------------------------------------------------
    void InnerContext::inline_child_task(TaskOp *child)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, INLINE_CHILD_TASK_CALL);
      // Remove this child from our context
      unregister_child_operation(child);
      // Check to see if the child is predicated
      // If it is wait for it to resolve
      if (child->is_predicated())
      {
        // See if the predicate speculates false, if so return false
        // and then we are done.
        if (!child->get_predicate_value(executing_processor))
          return;
      }
      // Save the state of our physical regions
      std::vector<bool> phy_regions_mapped(physical_regions.size());
      for (unsigned idx = 0; idx < physical_regions.size(); idx++)
        phy_regions_mapped[idx] = is_region_mapped(idx);
      // Inline the child task
      child->perform_inlining();
      // Now see if the mapping state of any of our
      // originally mapped regions has changed
      std::set<ApEvent> wait_events;
      for (unsigned idx = 0; idx < phy_regions_mapped.size(); idx++)
      {
        if (phy_regions_mapped[idx] && !is_region_mapped(idx))
        {
          // Need to remap
          MapOp *op = runtime->get_available_map_op(true);
          op->initialize(this, physical_regions[idx]);
          wait_events.insert(op->get_completion_event());
          runtime->add_to_dependence_queue(executing_processor, op);
        }
        else if (!phy_regions_mapped[idx] && is_region_mapped(idx))
        {
          // Need to unmap
          physical_regions[idx].impl->unmap_region();
        }
        // Otherwise everything is still the same
      }
      if (!wait_events.empty())
      {
        ApEvent wait_on = Runtime::merge_events(wait_events);
        if (!wait_on.has_triggered())
          wait_on.wait();
      }
    }

    //--------------------------------------------------------------------------
    VariantImpl* InnerContext::select_inline_variant(TaskOp *child) const
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, SELECT_INLINE_VARIANT_CALL);
      Mapper::SelectVariantInput input;
      Mapper::SelectVariantOutput output;
      input.processor = executing_processor;
      input.chosen_instances.resize(child->regions.size());
      // Compute the parent indexes since we're going to need them
      child->compute_parent_indexes();
      // Find the instances for this child
      for (unsigned idx = 0; idx < child->regions.size(); idx++)
      {
        // We can get access to physical_regions without the
        // lock because we know we are running in the application
        // thread in order to do this inlining
        unsigned local_index = child->find_parent_index(idx); 
#ifdef DEBUG_LEGION
        assert(local_index < physical_regions.size());
#endif
        InstanceSet instances;
        physical_regions[local_index].impl->get_references(instances);
        std::vector<MappingInstance> &mapping_instances = 
          input.chosen_instances[idx];
        mapping_instances.resize(instances.size());
        for (unsigned idx2 = 0; idx2 < instances.size(); idx2++)
        {
          mapping_instances[idx2] = 
            MappingInstance(instances[idx2].get_manager());
        }
      }
      output.chosen_variant = 0;
      // Always do this with the child mapper
      MapperManager *child_mapper = runtime->find_mapper(executing_processor,
                                                         child->map_id);
      child_mapper->invoke_select_task_variant(child, &input, &output);
      VariantImpl *variant_impl= runtime->find_variant_impl(child->task_id,
                                  output.chosen_variant, true/*can fail*/);
      if (variant_impl == NULL)
      {
        log_run.error("Invalid mapper output from invoction of "
                      "'select_task_variant' on mapper %s. Mapper selected "
                      "an invalidate variant ID %ld for inlining of task %s "
                      "(UID %lld).", child_mapper->get_mapper_name(),
                      output.chosen_variant, child->get_task_name(), 
                      child->get_unique_id());
#ifdef DEBUG_LEGION
        assert(false);
#endif
        exit(ERROR_INVALID_MAPPER_OUTPUT);
      }
      return variant_impl;
    }
    
    /////////////////////////////////////////////////////////////
    // Top Level Context 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    TopLevelContext::TopLevelContext(Runtime *rt, UniqueID ctx_id)
      : InnerContext(rt, NULL, dummy_requirements, dummy_indexes, dummy_mapped),
        context_uid(ctx_id)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    TopLevelContext::TopLevelContext(const TopLevelContext &rhs)
      : InnerContext(NULL, NULL, 
          dummy_requirements, dummy_indexes, dummy_mapped), context_uid(0)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    TopLevelContext::~TopLevelContext(void)
    //--------------------------------------------------------------------------
    { 
      // Tell the runtime that another top level task is done
      runtime->decrement_outstanding_top_level_tasks();
    }

    //--------------------------------------------------------------------------
    TopLevelContext& TopLevelContext::operator=(const TopLevelContext &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    int TopLevelContext::get_depth(void) const
    //--------------------------------------------------------------------------
    {
      return -1;
    }

    //--------------------------------------------------------------------------
    void TopLevelContext::pack_remote_context(Serializer &rez, 
                                              AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      rez.serialize<bool>(true); // top level context, all we need to pack
    }

    //--------------------------------------------------------------------------
    TaskContext* TopLevelContext::find_parent_context(void)
    //--------------------------------------------------------------------------
    {
      return NULL;
    }

    //--------------------------------------------------------------------------
    UniqueID TopLevelContext::get_context_uid(void) const
    //--------------------------------------------------------------------------
    {
      return context_uid;
    }

    //--------------------------------------------------------------------------
    AddressSpaceID TopLevelContext::get_version_owner(RegionTreeNode *node, 
                                                      AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      // We're the top-level task, so we handle the request on the node
      // that made the region
      const AddressSpaceID owner_space = node->get_owner_space();
      if (owner_space == runtime->address_space)
        return InnerContext::get_version_owner(node, source);
#ifdef DEBUG_LEGION
      assert(source == runtime->address_space); // should always be local
#endif
      // See if we already have it, or we already sent a request for it
      bool send_request = false;
      RtEvent wait_on;
      {
        AutoLock ctx_lock(context_lock);
        std::map<RegionTreeNode*,
                 std::pair<AddressSpaceID,bool> >::const_iterator finder =
          region_tree_owners.find(node);
        if (finder != region_tree_owners.end())
          return finder->second.first;
        // See if we already have an outstanding request
        std::map<RegionTreeNode*,RtUserEvent>::const_iterator request_finder =
          pending_version_owner_requests.find(node);
        if (request_finder == pending_version_owner_requests.end())
        {
          // We haven't sent the request yet, so do that now
          RtUserEvent request_event = Runtime::create_rt_user_event();
          pending_version_owner_requests[node] = request_event;
          wait_on = request_event;
          send_request = true;
        }
        else
          wait_on = request_finder->second;
      }
      if (send_request)
      {
        Serializer rez;
        rez.serialize(context_uid);
        rez.serialize<InnerContext*>(this);
        if (node->is_region())
        {
          rez.serialize<bool>(true);
          rez.serialize(node->as_region_node()->handle);
        }
        else
        {
          rez.serialize<bool>(false);
          rez.serialize(node->as_partition_node()->handle);
        }
        // Send it to the owner space 
        runtime->send_version_owner_request(owner_space, rez);
      }
      wait_on.wait();
      // Retake the lock in read-only mode and get the answer
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      std::map<RegionTreeNode*,
               std::pair<AddressSpaceID,bool> >::const_iterator finder = 
        region_tree_owners.find(node);
#ifdef DEBUG_LEGION
      assert(finder != region_tree_owners.end());
#endif
      return finder->second.first;
    }

    //--------------------------------------------------------------------------
    InnerContext* TopLevelContext::find_outermost_local_context(
                                                         InnerContext *previous)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(previous != NULL);
#endif
      return previous;
    }

    //--------------------------------------------------------------------------
    InnerContext* TopLevelContext::find_top_context(void)
    //--------------------------------------------------------------------------
    {
      return this;
    }

    /////////////////////////////////////////////////////////////
    // Remote Task 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    RemoteTask::RemoteTask(RemoteContext *own)
      : owner(own), context_index(0)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    RemoteTask::RemoteTask(const RemoteTask &rhs)
      : owner(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    RemoteTask::~RemoteTask(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    RemoteTask& RemoteTask::operator=(const RemoteTask &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    UniqueID RemoteTask::get_unique_id(void) const
    //--------------------------------------------------------------------------
    {
      return owner->get_context_uid();
    }

    //--------------------------------------------------------------------------
    unsigned RemoteTask::get_context_index(void) const
    //--------------------------------------------------------------------------
    {
      return context_index;
    }

    //--------------------------------------------------------------------------
    void RemoteTask::set_context_index(unsigned index)
    //--------------------------------------------------------------------------
    {
      context_index = index;
    }
    
    //--------------------------------------------------------------------------
    int RemoteTask::get_depth(void) const
    //--------------------------------------------------------------------------
    {
      return owner->get_depth();
    }

    //--------------------------------------------------------------------------
    const char* RemoteTask::get_task_name(void) const
    //--------------------------------------------------------------------------
    {
      TaskImpl *task_impl = owner->runtime->find_task_impl(task_id);
      return task_impl->get_name();
    }
    
    /////////////////////////////////////////////////////////////
    // Remote Context 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    RemoteContext::RemoteContext(Runtime *rt, UniqueID context_uid)
      : InnerContext(rt, NULL, remote_task.regions, local_parent_req_indexes,
          local_virtual_mapped), remote_owner_uid(context_uid), 
        parent_ctx(NULL), depth(-1), top_level_context(false), 
        remote_task(RemoteTask(this))
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    RemoteContext::RemoteContext(const RemoteContext &rhs)
      : InnerContext(NULL, NULL, rhs.regions, local_parent_req_indexes,
          local_virtual_mapped), remote_owner_uid(0), 
        remote_task(RemoteTask(this))
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    RemoteContext::~RemoteContext(void)
    //--------------------------------------------------------------------------
    {
      // Invalidate our context if necessary before deactivating
      // the wrapper as it will release the context
      if (!top_level_context)
      {
#ifdef DEBUG_LEGION
        assert(regions.size() == virtual_mapped.size());
#endif
        // Deactivate any region trees that we didn't virtually map
        for (unsigned idx = 0; idx < regions.size(); idx++)
          if (!virtual_mapped[idx])
            runtime->forest->invalidate_versions(tree_context, 
                                                 regions[idx].region);
      }
      else
        runtime->forest->invalidate_all_versions(tree_context);
    }

    //--------------------------------------------------------------------------
    RemoteContext& RemoteContext::operator=(const RemoteContext &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    int RemoteContext::get_depth(void) const
    //--------------------------------------------------------------------------
    {
      return depth;
    }

    //--------------------------------------------------------------------------
    Task* RemoteContext::get_task(void)
    //--------------------------------------------------------------------------
    {
      return &remote_task;
    }

    //--------------------------------------------------------------------------
    InnerContext* RemoteContext::find_outermost_local_context(
                                                         InnerContext *previous)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(previous != NULL);
#endif
      return previous;
    }
    
    //--------------------------------------------------------------------------
    InnerContext* RemoteContext::find_top_context(void)
    //--------------------------------------------------------------------------
    {
      if (top_level_context)
        return this;
      return find_parent_context()->find_top_context();
    }
    
    //--------------------------------------------------------------------------
    UniqueID RemoteContext::get_context_uid(void) const
    //--------------------------------------------------------------------------
    {
      return remote_owner_uid;
    }

    //--------------------------------------------------------------------------
    VersionInfo& RemoteContext::get_version_info(unsigned idx)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!top_level_context);
      assert(idx < version_infos.size());
#endif
      return version_infos[idx];
    }

    //--------------------------------------------------------------------------
    const std::vector<VersionInfo>* RemoteContext::get_version_infos(void)
    //--------------------------------------------------------------------------
    {
#ifdef DEBUG_LEGION
      assert(!top_level_context);
#endif
      return &version_infos;
    }

    //--------------------------------------------------------------------------
    TaskContext* RemoteContext::find_parent_context(void)
    //--------------------------------------------------------------------------
    {
      if (top_level_context)
        return NULL;
      // See if we already have it
      if (parent_ctx != NULL)
        return parent_ctx;
#ifdef DEBUG_LEGION
      assert(parent_context_uid != 0);
#endif
      // THIS IS ONLY SAFE BECAUSE THIS FUNCTION IS NEVER CALLED BY
      // A MESSAGE IN THE CONTEXT_VIRTUAL_CHANNEL
      parent_ctx = runtime->find_context(parent_context_uid);
#ifdef DEBUG_LEGION
      assert(parent_ctx != NULL);
#endif
      remote_task.parent_task = parent_ctx->get_task();
      return parent_ctx;
    }

    //--------------------------------------------------------------------------
    AddressSpaceID RemoteContext::get_version_owner(RegionTreeNode *node,
                                                    AddressSpaceID source)
    //--------------------------------------------------------------------------
    {
      const AddressSpaceID owner_space = node->get_owner_space(); 
      // If we are the top-level context then we handle the request
      // on the node that made the region
      if (top_level_context && (owner_space == runtime->address_space))
        return InnerContext::get_version_owner(node, source);
        // Otherwise we fall through and issue the request to the 
        // the node that actually made the region
#ifdef DEBUG_LEGION
      assert(source == runtime->address_space); // should always be local
#endif
      // See if we already have it, or we already sent a request for it
      bool send_request = false;
      RtEvent wait_on;
      {
        AutoLock ctx_lock(context_lock);
        std::map<RegionTreeNode*,
                 std::pair<AddressSpaceID,bool> >::const_iterator finder =
          region_tree_owners.find(node);
        if (finder != region_tree_owners.end())
          return finder->second.first;
        // See if we already have an outstanding request
        std::map<RegionTreeNode*,RtUserEvent>::const_iterator request_finder =
          pending_version_owner_requests.find(node);
        if (request_finder == pending_version_owner_requests.end())
        {
          // We haven't sent the request yet, so do that now
          RtUserEvent request_event = Runtime::create_rt_user_event();
          pending_version_owner_requests[node] = request_event;
          wait_on = request_event;
          send_request = true;
        }
        else
          wait_on = request_finder->second;
      }
      if (send_request)
      {
        Serializer rez;
        rez.serialize(remote_owner_uid);
        rez.serialize<InnerContext*>(this);
        if (node->is_region())
        {
          rez.serialize<bool>(true);
          rez.serialize(node->as_region_node()->handle);
        }
        else
        {
          rez.serialize<bool>(false);
          rez.serialize(node->as_partition_node()->handle);
        }
        // Send it to the owner space if we are the top-level context
        // otherwise we send it to the owner of the context
        const AddressSpaceID target = top_level_context ? owner_space :  
                          runtime->get_runtime_owner(remote_owner_uid);
        runtime->send_version_owner_request(target, rez);
      }
      wait_on.wait();
      // Retake the lock in read-only mode and get the answer
      AutoLock ctx_lock(context_lock,1,false/*exclusive*/);
      std::map<RegionTreeNode*,
               std::pair<AddressSpaceID,bool> >::const_iterator finder = 
        region_tree_owners.find(node);
#ifdef DEBUG_LEGION
      assert(finder != region_tree_owners.end());
#endif
      return finder->second.first;
    }

    //--------------------------------------------------------------------------
    void RemoteContext::unpack_remote_context(Deserializer &derez,
                                              std::set<RtEvent> &preconditions)
    //--------------------------------------------------------------------------
    {
      DETAILED_PROFILER(runtime, REMOTE_UNPACK_CONTEXT_CALL);
      derez.deserialize(top_level_context);
      // If we're the top-level context then we're already done
      if (top_level_context)
        return;
      derez.deserialize(depth);
      WrapperReferenceMutator mutator(preconditions);
      remote_task.unpack_external_task(derez, runtime, &mutator);
      local_parent_req_indexes.resize(remote_task.regions.size()); 
      for (unsigned idx = 0; idx < local_parent_req_indexes.size(); idx++)
        derez.deserialize(local_parent_req_indexes[idx]);
      size_t num_virtual;
      derez.deserialize(num_virtual);
      local_virtual_mapped.resize(regions.size(), false);
      for (unsigned idx = 0; idx < num_virtual; idx++)
      {
        unsigned index;
        derez.deserialize(index);
        local_virtual_mapped[index] = true;
      }
      version_infos.resize(regions.size());
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        if (virtual_mapped[idx])
          version_infos[idx].unpack_version_info(derez, runtime, preconditions);
        else
          version_infos[idx].unpack_version_numbers(derez, runtime->forest);
      }
      size_t num_local;
      derez.deserialize(num_local);
      local_fields.resize(num_local);
      for (unsigned idx = 0; idx < num_local; idx++)
      {
        derez.deserialize(local_fields[idx]);
        allocate_local_field(local_fields[idx]);
      }
      derez.deserialize(remote_completion_event);
      derez.deserialize(parent_context_uid);
      // See if we can find our parent task, if not don't worry about it
      // DO NOT CHANGE THIS UNLESS YOU THINK REALLY HARD ABOUT VIRTUAL 
      // CHANNELS AND HOW CONTEXT META-DATA IS MOVED!
      parent_ctx = runtime->find_context(parent_context_uid, true/*can fail*/);
      if (parent_ctx != NULL)
        remote_task.parent_task = parent_ctx->get_task();
    }

    /////////////////////////////////////////////////////////////
    // Leaf Context 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    LeafContext::LeafContext(Runtime *rt, TaskOp *owner)
      : TaskContext(rt, owner, owner->regions)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LeafContext::LeafContext(const LeafContext &rhs)
      : TaskContext(NULL, NULL, rhs.regions)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    LeafContext::~LeafContext(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    LeafContext& LeafContext::operator=(const LeafContext &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    RegionTreeContext LeafContext::get_context(void) const
    //--------------------------------------------------------------------------
    {
      assert(false);
      return RegionTreeContext();
    }

    //--------------------------------------------------------------------------
    ContextID LeafContext::get_context_id(void) const
    //--------------------------------------------------------------------------
    {
      assert(false);
      return 0;
    }

    //--------------------------------------------------------------------------
    void LeafContext::pack_remote_context(Serializer &rez,AddressSpaceID target)
    //--------------------------------------------------------------------------
    {
      assert(false);
    }

    //--------------------------------------------------------------------------
    bool LeafContext::attempt_children_complete(void)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      if (!children_complete_invoked)
      {
        children_complete_invoked = true;
        return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    bool LeafContext::attempt_children_commit(void)
    //--------------------------------------------------------------------------
    {
      AutoLock ctx_lock(context_lock);
      if (!children_commit_invoked)
      {
        children_commit_invoked = true;
        return true;
      }
      return false;
    }

    //--------------------------------------------------------------------------
    void LeafContext::inline_child_task(TaskOp *child)
    //--------------------------------------------------------------------------
    {
      assert(false);
    }

    //--------------------------------------------------------------------------
    VariantImpl* LeafContext::select_inline_variant(TaskOp *child) const
    //--------------------------------------------------------------------------
    {
      assert(false);
      return NULL;
    }

    //--------------------------------------------------------------------------
    bool LeafContext::is_leaf_context(void) const
    //--------------------------------------------------------------------------
    {
      return true;
    }

    /////////////////////////////////////////////////////////////
    // Inline Context 
    /////////////////////////////////////////////////////////////

    //--------------------------------------------------------------------------
    InlineContext::InlineContext(Runtime *rt, TaskContext *enc, TaskOp *child)
      : TaskContext(rt, child, child->regions), 
        enclosing(enc), inline_task(child)
    //--------------------------------------------------------------------------
    {
      executing_processor = enclosing->get_executing_processor();
      physical_regions.resize(regions.size());
      parent_req_indexes.resize(regions.size());
      // Now update the parent regions so that they are valid with
      // respect to the outermost context
      for (unsigned idx = 0; idx < regions.size(); idx++)
      {
        unsigned index = enclosing->find_parent_region(idx, child);
        parent_req_indexes[idx] = index;
        if (index < enclosing->regions.size())
        {
          child->regions[idx].parent = enclosing->regions[index].parent;
          physical_regions[idx] = enclosing->get_physical_region(index);
        }
        else
        {
          // This is a created requirements, so we have to make a copy
          RegionRequirement copy;
          enclosing->clone_requirement(index, copy);
          child->regions[idx].parent = copy.parent;
          // physical regions are empty becaue they are virtual
        }
      }
    }

    //--------------------------------------------------------------------------
    InlineContext::InlineContext(const InlineContext &rhs)
      : TaskContext(NULL, NULL, rhs.regions), enclosing(NULL), inline_task(NULL)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
    }

    //--------------------------------------------------------------------------
    InlineContext::~InlineContext(void)
    //--------------------------------------------------------------------------
    {
    }

    //--------------------------------------------------------------------------
    InlineContext& InlineContext::operator=(const InlineContext &rhs)
    //--------------------------------------------------------------------------
    {
      // should never be called
      assert(false);
      return *this;
    }

    //--------------------------------------------------------------------------
    const std::vector<PhysicalRegion>& InlineContext::begin_task(void)
    //--------------------------------------------------------------------------
    {
      return enclosing->get_physical_regions();
    }

    //--------------------------------------------------------------------------
    void InlineContext::end_task(const void *res, size_t res_size, bool owned)
    //--------------------------------------------------------------------------
    {
      inline_task->end_inline_task(res, res_size, owned);
    }

  };
};

// EOF
