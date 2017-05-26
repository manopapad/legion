/* Copyright 2015 Stanford University, NVIDIA Corporation
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

// preimage operations for Realm dependent partitioning

#ifndef REALM_DEPPART_PREIMAGE_H
#define REALM_DEPPART_PREIMAGE_H

#include "partitions.h"

namespace Realm {

  template <int N, typename T, int N2, typename T2>
  class PreimageMicroOp : public PartitioningMicroOp {
  public:
    static const int DIM = N;
    typedef T IDXTYPE;
    static const int DIM2 = N2;
    typedef T2 IDXTYPE2;

    static const Opcode OPCODE = UOPCODE_PREIMAGE;

    static DynamicTemplates::TagType type_tag(void);

    PreimageMicroOp(ZIndexSpace<N,T> _parent_space, ZIndexSpace<N,T> _inst_space,
		    RegionInstance _inst, size_t _field_offset, bool _is_ranged);
    virtual ~PreimageMicroOp(void);

    void add_sparsity_output(ZIndexSpace<N2,T2> _target, SparsityMap<N,T> _sparsity);

    virtual void execute(void);

    void dispatch(PartitioningOperation *op, bool inline_ok);

  protected:
    friend struct RemoteMicroOpMessage;
    template <typename S>
    bool serialize_params(S& s) const;

    // construct from received packet
    template <typename S>
    PreimageMicroOp(gasnet_node_t _requestor, AsyncMicroOp *_async_microop, S& s);

    template <typename BM>
    void populate_bitmasks_ptrs(std::map<int, BM *>& bitmasks);

    template <typename BM>
    void populate_bitmasks_ranges(std::map<int, BM *>& bitmasks);

    ZIndexSpace<N,T> parent_space, inst_space;
    RegionInstance inst;
    size_t field_offset;
    bool is_ranged;
    std::vector<ZIndexSpace<N2,T2> > targets;
    std::vector<SparsityMap<N,T> > sparsity_outputs;
  };

  template <int N, typename T, int N2, typename T2>
  class PreimageOperation : public PartitioningOperation {
  public:
    PreimageOperation(const ZIndexSpace<N,T>& _parent,
		      const std::vector<FieldDataDescriptor<ZIndexSpace<N,T>,ZPoint<N2,T2> > >& _field_data,
		      const ProfilingRequestSet &reqs,
		      Event _finish_event);

    PreimageOperation(const ZIndexSpace<N,T>& _parent,
		      const std::vector<FieldDataDescriptor<ZIndexSpace<N,T>,ZRect<N2,T2> > >& _field_data,
		      const ProfilingRequestSet &reqs,
		      Event _finish_event);

    virtual ~PreimageOperation(void);

    ZIndexSpace<N,T> add_target(const ZIndexSpace<N2,T2>& target);

    virtual void execute(void);

    virtual void print(std::ostream& os) const;

    virtual void set_overlap_tester(void *tester);

    void provide_sparse_image(int index, const ZRect<N2,T2> *rects, size_t count);

  protected:
    ZIndexSpace<N,T> parent;
    std::vector<FieldDataDescriptor<ZIndexSpace<N,T>,ZPoint<N2,T2> > > ptr_data;
    std::vector<FieldDataDescriptor<ZIndexSpace<N,T>,ZRect<N2,T2> > > range_data;
    std::vector<ZIndexSpace<N2,T2> > targets;
    std::vector<SparsityMap<N,T> > preimages;
    GASNetHSL mutex;
    OverlapTester<N2,T2> *overlap_tester;
    std::map<int, std::vector<ZRect<N2,T2> > > pending_sparse_images;
    int remaining_sparse_images;
    std::vector<int> contrib_counts;
    AsyncMicroOp *dummy_overlap_uop;
  };
    
  struct ApproxImageResponseMessage {
    struct RequestArgs : public BaseMedium {
      DynamicTemplates::TagType type_tag;
      intptr_t approx_output_op;
      int approx_output_index;
    };

    struct DecodeHelper {
      template <typename NT, typename T, typename N2T, typename T2>
      static void demux(const RequestArgs *args, const void *data, size_t datalen);
    };

    static void handle_request(RequestArgs args, const void *data, size_t datalen);

    typedef ActiveMessageMediumNoReply<APPROX_IMAGE_RESPONSE_MSGID,
                                       RequestArgs,
                                       handle_request> Message;

    template <int N, typename T, int N2, typename T2>
    static void send_request(gasnet_node_t target, intptr_t output_op, int output_index,
			     const ZRect<N2,T2> *rects, size_t count);
  };

  template <int N, typename T, int N2, typename T2>
  /*static*/ void ApproxImageResponseMessage::send_request(gasnet_node_t target, 
							   intptr_t output_op, int output_index,
							   const ZRect<N2,T2> *rects, size_t count)
  {
    RequestArgs args;

    args.type_tag = NTNT_TemplateHelper::encode_tag<N,T,N2,T2>();
    args.approx_output_op = output_op;
    args.approx_output_index = output_index;

    Message::request(target, args, rects, count * sizeof(ZRect<N2,T2>), PAYLOAD_COPY);
  }
    
};

#endif // REALM_DEPPART_PREIMAGE_H
