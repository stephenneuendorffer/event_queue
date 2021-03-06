//===- CommandProcessor.cpp -------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EQueue/CommandProcessor.h"

#include "EQueue/EQueueDialect.h"
#include "EQueue/EQueueOps.h"
#include "EQueue/EQueueTraits.h"
#include "EQueue/EQueueStructs.h"

#include <list>
#include <deque>
#include <vector>
#include <map>
#include <sstream>
#include <string>
#include <float.h>

#define INDEX_WIDTH 32
#define DEBUG_TYPE "command_processor"
static bool verbose = false;
using namespace mlir;
namespace acdc {

//using ExecFunc = void (*)(T, std::vector<llvm::Any> &, std::vector<llvm::Any> &);//op, in, out
struct Visitor : VisitorInterface{
	void Visit(Executor<mlir::ConstantIndexOp>& exec) override {

		auto attr = exec.op.getAttrOfType<mlir::IntegerAttr>("value");
		exec.out[0] = attr.getValue().sextOrTrunc(INDEX_WIDTH);
	}
};

class Runner {

  const int TRACE_PID_QUEUE=0;
  const int TRACE_PID_ALLOC=1;
  const int TRACE_PID_EQUEUE=2;


#if 0
void executeOp(mlir::Op op,
               std::vector<llvm::Any> &in,
               std::vector<llvm::Any> &out) {
}
#endif

public:

  Runner(std::ostream &trace_stream) : traceStream(trace_stream), time(1), deviceId(0)
  {
  }



std::string printAnyValueWithType(mlir::Type type, llvm::Any &value) {
  std::stringstream out;
  if(type.isa<mlir::IntegerType>() ||
     type.isa<mlir::IndexType>()) {
    out << llvm::any_cast<llvm::APInt>(value).getSExtValue();
    return out.str();
  } else if(type.isa<mlir::FloatType>()) {
    out << llvm::any_cast<llvm::APFloat>(value).convertToDouble();
    return out.str();
  } else if(type.isa<mlir::NoneType>()) {
    return "none";
  } else {
    llvm_unreachable("Unknown result type!");
  }
}


void emitTraceStart(std::ostream &s)
{
  s << "[\n";
}

void emitTraceEnd(std::ostream &s)
{
  s << "{}]\n";
}

void emitTraceEvent(std::ostream &s,
                    std::string name,
                    std::string cat,
                    std::string ph,
                    int64_t start_time,
                    int64_t tid,
                    int64_t pid) {
  s << "{\n";
  s << "  \"name\": \"" << name << "\"," << "\n";
  s << "  \"cat\": \""<< cat << "\"," << "\n";
  s << "  \"ph\": \""<< ph << "\"," << "\n";
  s << "  \"ts\": " << start_time << "," << "\n";
  s << "  \"pid\": " << pid << "," << "\n";
  s << "  \"tid\": " << tid << "," << "\n";
  s << "  \"args\": " << "{}" << "" << "\n";
  s << "},\n";
}


xilinx::equeue::MemAllocOp getAllocOp(Value memRef){
  return valueIds[memRef].getDefiningOp<xilinx::equeue::MemAllocOp>();
}
int getMemVolume(mlir::Value memRef){
  auto allocOp = getAllocOp(memRef);
  int dlines = 1;
  for (auto s : allocOp.getShape()){
    dlines *= s;
  }
  return dlines;
}

uint64_t modelOp(const uint64_t &time, OpEntry &c)
{
  LLVM_DEBUG(llvm::dbgs()<<"[modelOp] start model op\n");
  mlir::Operation *op = c.op;
  uint64_t execution_time = 1;
  if (auto Op = mlir::dyn_cast<xilinx::equeue::CreateMemOp>(op)) {
    auto shape = Op.getShape();
    int dlines = 1;
    for (auto s : shape){
      dlines *= s;
    }
    auto dtype = Op.getDataType().str();
    auto key = valueIds[op->getResults()[0]];
    if (Op.getMemType() == "DRAM")
      deviceMap[key] = std::make_unique<xilinx::equeue::DRAM>(deviceId++, dlines, dtype);
    else if (Op.getMemType() == "SRAM")
      deviceMap[key] = std::make_unique<xilinx::equeue::SRAM>(deviceId++, dlines, dtype);
    else
      llvm_unreachable("No such memory type.\n");
  }
  else if (auto Op = mlir::dyn_cast<xilinx::equeue::CreateDMAOp>(op)) {
    auto key = valueIds[op->getResults()[0]];
    deviceMap[key] = std::make_unique<xilinx::equeue::DMA>(deviceId++);
  }
  else if (auto Op = mlir::dyn_cast<xilinx::equeue::MemReadOp>(op)) {
    int dlines = Op.hasOffset() ? 1 : getMemVolume( Op.getBuffer() );
    auto key = valueIds[getAllocOp(Op.getBuffer()).getMemHandler()];
    auto mem = static_cast<xilinx::equeue::Memory *>(deviceMap[key].get());
    c.mem_tids.push_back(mem->uid);
    execution_time = mem->getReadOrWriteCycles(dlines, xilinx::equeue::MemOp::Read);
    return mem->scheduleEvent(time, execution_time, true);
  }
  else if (auto Op = mlir::dyn_cast<xilinx::equeue::MemWriteOp>(op)) {
    int dlines = getMemVolume( Op.getBuffer() );
    auto key = valueIds[getAllocOp(Op.getBuffer()).getMemHandler()];
    auto mem = static_cast<xilinx::equeue::Memory *>(deviceMap[key].get());
    c.mem_tids.push_back(mem->uid);
    execution_time = mem->getReadOrWriteCycles(dlines, xilinx::equeue::MemOp::Write);
    return mem->scheduleEvent(time, execution_time, true);
  }
  else if (auto Op = mlir::dyn_cast<xilinx::equeue::MemCopyOp>(op)) {
    //TODO: calculate offset
    int srcLines = getMemVolume( Op.getSrcBuffer() );
    int destLines = getMemVolume( Op.getDestBuffer() );
    int dlines = std::min(srcLines, destLines);
    auto srcKey = valueIds[getAllocOp(Op.getSrcBuffer()).getMemHandler()];
    auto destKey = valueIds[getAllocOp(Op.getDestBuffer()).getMemHandler()];
    auto srcMem = static_cast<xilinx::equeue::Memory *>(deviceMap[srcKey].get());
    c.mem_tids.push_back(srcMem->uid);
    uint64_t readTime = srcMem->getReadOrWriteCycles(dlines, xilinx::equeue::MemOp::Read);
    auto destMem = static_cast<xilinx::equeue::Memory *>(deviceMap[destKey].get());
    c.mem_tids.push_back(destMem->uid);
    uint64_t writeTime = destMem->getReadOrWriteCycles(dlines, xilinx::equeue::MemOp::Write);
    int total_size = srcMem->total_size;
    int volume = dlines * total_size;
    auto key = valueIds[Op.getDMAHandler()];
    auto dma = static_cast<xilinx::equeue::DMA *>(deviceMap[key].get());
    uint64_t dmaTime = dma->getTransferCycles(volume);
    execution_time = std::max({readTime, writeTime, dmaTime});
    //clean outdated events
    return dma->scheduleEvent(time, execution_time, {destMem, srcMem});
  }
  if (  op->hasTrait<mlir::OpTrait::StructureOpTrait>() ||
        mlir::dyn_cast<mlir::ConstantOp>(op) ||
        mlir::dyn_cast<xilinx::equeue::AwaitOp>(op) ||
        mlir::dyn_cast<xilinx::equeue::LaunchOp>(op) ||
        mlir::dyn_cast<xilinx::equeue::ReturnOp>(op) ||
        mlir::dyn_cast<mlir::scf::ForOp>(op) ||
        mlir::dyn_cast<mlir::scf::YieldOp>(op) ||
        mlir::dyn_cast<mlir::ReturnOp>(op) ){
    execution_time = 0;
  }
  return execution_time+time;
}

std::string to_string(Operation *op) {
  return op ? op->getName().getStringRef().str() : "nop";
}

std::string to_string(OpEntry &c) {
  return to_string(c.op);
}


//update value if the value has signalType
void updateExecution(mlir::ValueRange args){
  for (Value arg: args)
    if( arg.getType().isa<xilinx::equeue::EQueueSignalType>() ){
      valueMap[valueIds[arg]]++;
    }
}
//update signal to its definer
void updateSignalIds(mlir::ValueRange args0, mlir::ValueRange args1){
  auto arg1_it = args1.begin();
  for (Value arg0: args0 ){
    if( arg0.getType().isa<xilinx::equeue::EQueueSignalType>() ){
      signalIds[valueIds[arg0]] = getSignalId(valueIds[*arg1_it]);
    }
    arg1_it += 1;
  }
}
//TODO: illustrate this
void updateIterState(mlir::ValueRange args, bool yield){
  for (Value arg: args ){
    if( arg.getType().isa<xilinx::equeue::EQueueSignalType>() ){
      iterState[valueIds[arg]] = yield;
    }
  }
}
void finishOp(LauncherTable &l, uint64_t time, uint64_t pid)
{
  if (l.is_idle()) return;
  LLVM_DEBUG(llvm::dbgs()<<to_string(l.op_entry.op)<<": not idle\n");

  auto &c = l.op_entry;
  if (c.is_started()) {
    if (c.is_done(time)) {
      if (verbose) {
        llvm::outs() << "finish: '";
        //c.op->print(llvm::outs());
        llvm::outs()<<to_string(c.op);
        llvm::outs() << "' @ " << time << "\n";
      }

      LLVM_DEBUG(llvm::dbgs() << "OP:  " << c.op->getName() << "\n");
      if (auto Op = mlir::dyn_cast<xilinx::equeue::MemCopyOp>(c.op)){
        updateExecution( c.op->getResults() );
      }
      if (auto Op = mlir::dyn_cast<xilinx::equeue::LaunchOp>(c.op)){
        updateSignalIds( Op.getBody()->getArguments(), Op.getLaunchOperands() );
      }else if (auto Op = mlir::dyn_cast<xilinx::equeue::ReturnOp>(c.op)) {
        // increment launchOp && its results
        updateExecution( c.op->getParentOp()->getResult(0) );
        updateSignalIds( c.op->getParentOp()->getResults().drop_front(), c.op->getOperands() );
      }
      else if (auto Op = mlir::dyn_cast<mlir::scf::ForOp>(c.op)){
        updateSignalIds( Op.getRegionIterArgs(), Op.getIterOperands() );
        updateIterState( Op.getRegionIterArgs(), false );
      }
      else if (auto Op = mlir::dyn_cast<mlir::scf::YieldOp>(c.op)){
        if( exTimes[c.op] % getExTimes( c.op->getParentOp() ) == 0 ){
          updateSignalIds( c.op->getParentOp()->getResults(), c.op->getOperands() );
        }else{
          auto pop = mlir::dyn_cast<mlir::scf::ForOp>(c.op->getParentOp());
          updateSignalIds( pop.getRegionIterArgs(), c.op->getOperands() );
          updateIterState( pop.getRegionIterArgs(), true );
        }
      } else{
        if ( mlir::dyn_cast<xilinx::equeue::CreateProcOp>(c.op) ||
          mlir::dyn_cast<xilinx::equeue::CreateDMAOp>(c.op)){
          auto key = c.op->getResult(0);
          LauncherTable l;
          launchTables.insert({key, l});
        }
      }

      auto op_str = to_string(c)+std::to_string(c.tid);
      size_t position = op_str.find(op_str);
      auto opStr = op_str.substr(position);
      // emit trace event end
      if ( c.end_time != c.start_time ){
        emitTraceEvent(traceStream, opStr, "operation", "E", time, pid, 0);
      }
      for(auto iter = c.mem_tids.begin(); iter != c.mem_tids.end(); iter++){
        emitTraceEvent(traceStream, opStr, "memory", "E", time, *iter, 1);
      }

      // if (c.compute_xfer_cost && c.compute_op_cost) {
      //   if (c.compute_op_cost >= c.compute_xfer_cost) {
      //     emitTraceEvent(traceStream, "compute_bound", "equeue", "B", c.start_time, 0, TRACE_PID_EQUEUE);
      //     emitTraceEvent(traceStream, "compute_bound", "equeue", "E", c.end_time, 0, TRACE_PID_EQUEUE);
      //   }
      //   else {
      //     emitTraceEvent(traceStream, "memory_bound", "equeue", "B", c.start_time, 0, TRACE_PID_EQUEUE);
      //     emitTraceEvent(traceStream, "memory_bound", "equeue", "E", c.end_time, 0, TRACE_PID_EQUEUE);
      //   }
      // }

      // set op_entry to empty
      OpEntry entry;
      l.op_entry = entry;
    }else {
      // running...
      if (verbose) {
        llvm::outs() << "running: '";
        //c.op->print(llvm::outs());
	      llvm::outs()<<to_string(c.op);
        llvm::outs() << "' @ " << time << " - " << c.end_time << "\n";
      }
      // in-order, return.
    }
  }

}

void scheduleOp(LauncherTable &l, uint64_t time, uint64_t pid)
{
  if( l.is_idle() ) return;

  auto& c_next = l.op_entry;
  LLVM_DEBUG(llvm::dbgs()<<"[schedule] got c_next\n");
  if (!c_next.queue_ready_time)
    c_next.queue_ready_time = time;
  // emit trace event begin

  if( auto Op = llvm::dyn_cast<xilinx::equeue::AwaitOp>(c_next.op) )
    if(waitForSignal(c_next.op))
      return;
  LLVM_DEBUG(llvm::dbgs()<<"[schedule] not waiting for any signal\n");

  if ( !c_next.is_started() ){
    if( llvm::dyn_cast<xilinx::equeue::LaunchOp>(c_next.op) ||
        llvm::dyn_cast<xilinx::equeue::MemCopyOp>(c_next.op) ||
         llvm::dyn_cast<xilinx::equeue::AwaitOp>(c_next.op) ){
      opMap[c_next.op]++;
    }
    LLVM_DEBUG(llvm::dbgs()<<"[schedule] updated execution\n");
    c_next.start_time = time;
    c_next.end_time = modelOp(time, c_next);

    if (verbose) {
      llvm::outs()<<"scheduled: '";
      //c_next.op->print(llvm::outs());
      llvm::outs()<<to_string(c_next.op);
      llvm::outs() << "' @ " << c_next.start_time << " - " << c_next.end_time << "\n";
    }
    auto op_str = to_string(c_next)+std::to_string(c_next.tid);
    size_t position = op_str.find(op_str);
    auto opStr = op_str.substr(position);
    if ( c_next.end_time != c_next.start_time ){
      emitTraceEvent(traceStream, opStr, "operation", "B", time, pid, 0);
    }
    for(auto iter = c_next.mem_tids.begin(); iter != c_next.mem_tids.end(); iter++){
      emitTraceEvent(traceStream, opStr, "memory", "B", time, *iter, 1);
    }
    if (time > c_next.queue_ready_time) {
      emitTraceEvent(traceStream, "stall", "operation", "B", c_next.queue_ready_time, pid, 0);
      emitTraceEvent(traceStream, "stall", "operation", "E", time, pid, 0);
    }

  }
  return;
}


int64_t getConstant(mlir::Value v){
  if (auto constantOp = v.getDefiningOp<ConstantOp>())
    return constantOp.getValue().cast<IntegerAttr>().getInt();
  else
    llvm_unreachable("invalid for loop control argument");
}
/// get execution time of for loop, assuming all bounds are constant
int64_t getExTimes(mlir::Operation *op){
  int64_t lb = getConstant(op->getOperand(0));
  int64_t ub = getConstant(op->getOperand(1));
  int64_t step =  getConstant(op->getOperand(2));
  return int((ub - lb)/step);
}

mlir::Value getSignalId(mlir::Value in){
   return signalIds.count(in)? signalIds[in] : in;
}

bool waitForSignal(mlir::Operation* op){
  // check if the signals are all ready
  // if so, the operation is ready to be execute
  LLVM_DEBUG(llvm::dbgs()<<"[waitforsignal] "<<to_string(op)<<"\n");
  //auto op_block_cycle = blockExs[op->getBlock()];
  for( auto in : op->getOperands() ){
    if(in.getType().isa<xilinx::equeue::EQueueSignalType>()){
      if( waitForSignal(op, in) ) return true;
    }
  }
  return false;
}

bool waitForSignal(mlir::Operation* op, mlir::Value in){
  auto signal = getSignalId(valueIds[in]);
  auto op_block_cycle = blockExs[op->getBlock()];
  auto in_block_cycle = 1;
  LLVM_DEBUG(llvm::dbgs()<<"[waitforsignal] "<<signal.getDefiningOp()->getName()<<"\n");
  if(signal.getDefiningOp())
    in_block_cycle = blockExs[signal.getDefiningOp()->getBlock()];
  if( !valueMap.count( signal ) ){
    return ! (iterInitValue.count( valueIds[in] ) // the signal is iterator
        && iterInitValue[valueIds[in]] != signal // the signal is not initial_value
        && valueMap.count(iterInitValue[valueIds[in]] ) ); // the initial_signal is generated
  }
  if( (iterInitValue.count( valueIds[in] ) // the signal is iterator
      && iterInitValue[valueIds[in]] != signal) ){ // the signal is not initial_value
    auto init_signal = iterInitValue[valueIds[in]];
    auto init_value_cycle = 1;
    if(init_signal.getDefiningOp())
      init_value_cycle = blockExs[init_signal.getDefiningOp()->getBlock()];
    if( opMap[op] >= op_block_cycle * valueMap[init_signal] / init_value_cycle ){
      return true;
    }
    if(opMap[op] >= op_block_cycle * ( valueMap[signal] + 1) / in_block_cycle){
      return true;
    }
  }else{
    if(opMap[op] >= op_block_cycle * valueMap[signal] / in_block_cycle){
      return true;
    }
  }

  return false;
}

void checkEventQueue(LauncherTable& l){
  while( !l.event_queue.empty() ){
    auto op = l.event_queue.front();

    if(op->hasTrait<mlir::OpTrait::ControlOpTrait>()){
      if( waitForSignal(op) ) return;
      // the control operation has immediate effect
      opMap[op]++;
      updateExecution(op->getResults());
      // first event of event_queue will be handled by launcher
      // continue to check next one
      l.event_queue.erase(l.event_queue.begin());
      continue;
    }
    //mlir::Value launcher;
    if( auto Op = llvm::dyn_cast<xilinx::equeue::LaunchOp>(op) ){
      //TODO, only check start_signal
      if( waitForSignal(op, Op.getStartSignal()) ) return;
      //launcher = valueIds[Op.getDeviceHandler()];
    } else {//memcopy
      if( waitForSignal(op) ) return;
      //launcher = valueIds[Op.getDMAHandler()];
    }

    if( l.is_idle() ){
      // the first event of event_queue is ready at launcher
      // and launcher is idle to process it

      // the only way to get launchOp is through checkEventQueue
      // so we need to update next_iter and op_entry here
      OpEntry entry(op);
      l.op_entry = entry;
      LLVM_DEBUG(llvm::dbgs()<<"[launchee] added op_entry\n");
      if( auto Op = llvm::dyn_cast<xilinx::equeue::LaunchOp>(op) )
        l.set_block(Op.getBody());
      l.event_queue.erase(l.event_queue.begin());
      LLVM_DEBUG(llvm::dbgs()<<"[launchee] erased : "<<l.event_queue.size()<<"\n");
    }
    break;
  }
}

void setOpEntry(LauncherTable& l, uint64_t& tid){
    auto &opEntry = l.op_entry;
    if(!opEntry.op){
      while(true){
        if( !l.block || l.next_iter == l.block->end() ) break;
        LLVM_DEBUG(llvm::dbgs()<<"[set_op_entry] next op\n");
        auto op = &*l.next_iter;
        LLVM_DEBUG(llvm::dbgs()<<to_string(op)<<"\n");
        LauncherTable lt;
        if(op->hasTrait<mlir::OpTrait::AsyncOpTrait>()){
          // launch, memcpy, control...
          if(op->hasTrait<mlir::OpTrait::ControlOpTrait>()){
            if (l.add_event_queue(op)){
              l.next_iter++;
            }else
              break;
          }else{
            Value launcher;
            if( auto Op = llvm::dyn_cast<xilinx::equeue::LaunchOp>(op) ){
              //TODO, only check start_signal
              launcher = valueIds[Op.getDeviceHandler()];
            } else if ( auto Op = llvm::dyn_cast<xilinx::equeue::MemCopyOp>(op) ){
              launcher = valueIds[Op.getDMAHandler()];
            }
            auto& lt = launchTables[launcher];
            if(lt.add_event_queue(op)){
              l.next_iter++;
            }else
              break;
          }
        } else {
          OpEntry entry(op, tid++);
          l.op_entry=entry;
          if (auto Op = mlir::dyn_cast<mlir::scf::ForOp>(op)){
            l.set_block(Op.getBody());
          } else if ( auto Op = llvm::dyn_cast<mlir::scf::YieldOp>(op) ){
            exTimes[op]++;
            LLVM_DEBUG(llvm::dbgs()<<"[set_op_entry] forOp ex times: "<<exTimes[op]<<"\n");
            auto pop = op->getParentOp(); // forOp
            if (exTimes[op] % getExTimes(pop) == 0) {
              // exit for loop
              l.block = pop->getBlock();
              l.next_iter = ++mlir::Block::iterator(pop);
            }else{
              // redo for loop
              l.next_iter =  llvm::dyn_cast<mlir::scf::ForOp>(pop).getBody()->begin();
            }
          } else {
            l.next_iter++;
          }
          break;
        }
      }
    }
}

void nextEndTimes( LauncherTable &l, std::vector<uint64_t> &next_times){
  if ( !l.is_idle() && l.op_entry.is_started() ){
  	next_times.push_back(l.op_entry.end_time);
  }
}

void simulateFunction(mlir::FuncOp &toplevel)
{

  auto hostIter = toplevel.getCallableRegion()->front().begin();
  hostTable.next_iter = hostIter;
  hostTable.block = &toplevel.getCallableRegion()->front();

  time = 1;
  bool running = true;
  uint64_t tid = 0;
  while (running) {
    LLVM_DEBUG(llvm::dbgs()<<"1. setOpEntry\n");
    setOpEntry(hostTable, tid);
    for ( auto iter = launchTables.begin(); iter != launchTables.end(); iter++){
      LLVM_DEBUG(llvm::dbgs()<<iter->first<<":\n");
      setOpEntry(iter->second, tid);
    }

    LLVM_DEBUG(llvm::dbgs()<<"2. checkEventQueue\n");
    checkEventQueue(hostTable);
    for ( auto iter = launchTables.begin(); iter != launchTables.end(); iter++){
      checkEventQueue(iter->second);
    }
    // end condition, nothing can be put on to op_entry
    running = !hostTable.is_idle();
		for (auto iter = launchTables.begin(); iter!=launchTables.end(); iter++)
			running = running || !iter->second.is_idle();
    if( !running ) break;

    LLVM_DEBUG(llvm::dbgs()<<"3. scheduleOp\n");
    uint64_t pid = 0;
    scheduleOp(hostTable, time, pid++);
    for (auto iter = launchTables.begin(); iter!= launchTables.end(); iter++){
			scheduleOp(iter->second, time, pid++);
		}

    // find the closest time stamp currently running op is done.
    std::vector<uint64_t> next_times;
    nextEndTimes(hostTable, next_times);
		for (auto iter = launchTables.begin(); iter!=launchTables.end(); iter++){
      nextEndTimes(iter->second, next_times);
		}
    if(!next_times.size())
      time = time;
    else
      time =  *std::min_element(next_times.begin(), next_times.end());
    LLVM_DEBUG(llvm::dbgs()<<"Next end time: "<<time<<"\n");

    LLVM_DEBUG(llvm::dbgs()<<"4. finishOp\n");
    pid = 0;
    finishOp(hostTable, time, pid++);
    for (auto iter = launchTables.begin(); iter!= launchTables.end(); iter++){
			finishOp(iter->second, time, pid++);
		}
    LLVM_DEBUG(llvm::dbgs()<<"=================\n\n");
  }

}
template <typename FuncT>
void walkRegions(MutableArrayRef<Region> regions, const FuncT &func) {
  for (Region &region : regions)
    for (Block &block : region) {
      func(block);
      // Traverse all nested regions.
      for (Operation &operation : block)
        walkRegions(operation.getRegions(), func);
    }
}
  void printValueRef(const Value& value) {
    if (value.getDefiningOp())
      llvm::outs() << value.getDefiningOp()->getName();
    else {
      auto blockArg = value.cast<BlockArgument>();
      llvm::outs()  << "arg" << blockArg.getArgNumber() << "@b ";
      if (blockArg.getOwner()->getParentOp())
        llvm::outs()<< blockArg.getOwner()->getParentOp()->getName();
    }
    llvm::outs() << " ";
  };

/// link operands of launch with region arguments of launch region
/// so that a region argument is mapped to its defining Op
void buildIdMap(mlir::FuncOp &toplevel){
  walkRegions(*toplevel.getCallableRegion(), [&](Block &block) {
    // build iter init_value map
    auto pop = block.getParentOp();
    if( auto Op = llvm::dyn_cast<mlir::scf::ForOp>(pop) ) {
      auto arg_it = Op.getRegionIterArgs().begin();
      for ( Value operand : Op.getIterOperands() ){
        iterInitValue.insert({*arg_it, valueIds[operand]});
        arg_it += 1;
      }
    }
    //build value id map
    if( auto Op = llvm::dyn_cast<xilinx::equeue::LaunchOp>(pop) ) {
      auto arg_it = block.args_begin();
      for ( Value operand : Op.getLaunchOperands() ){
        valueIds.insert({*arg_it, valueIds[operand]});
        arg_it += 1;
      }
    } else {
      for (BlockArgument argument : block.getArguments())
        valueIds.insert({argument, argument});
    }
    for (Operation &operation : block) {
      for (Value result : operation.getResults())
        valueIds.insert({result, result});
    }
  });
}
void buildExMap(mlir::FuncOp &toplevel){
  walkRegions(*toplevel.getCallableRegion(), [&](Block &block) {
    auto pop = block.getParentOp();
    uint64_t ex_times = 1;
    if( auto Op = llvm::dyn_cast<mlir::scf::ForOp>(pop) ) {
      ex_times = getExTimes(pop);
    }
    if( blockExs.count(pop->getBlock()) )
      blockExs.insert({&block, blockExs[pop->getBlock()]*ex_times});
    else
      blockExs.insert({&block, ex_times});
  });
}
// todo private:
public:

  // The valueMap associates each SSA statement in the program
  // with the number of time the value is produced.
  llvm::DenseMap<mlir::Value, uint64_t> valueMap;
  // There is a lap between a value is consumed and a result is generated
  // e.g. %1 = memcpy(%0) immediately consumes %0, while %1 is still on-flight
  // we therefore need opMap to track if we have enough value to proceed
  llvm::DenseMap<mlir::Operation *, uint64_t> opMap;

  uint64_t deviceId;
  llvm::DenseMap<mlir::Value, std::unique_ptr<xilinx::equeue::Device> > deviceMap;

private:
  std::ostream &traceStream;

  uint64_t time;

  LauncherTable hostTable;
	llvm::DenseMap<mlir::Value, LauncherTable > launchTables;

  // map operation to (left) execution times
  llvm::DenseMap< mlir::Operation *, uint64_t > exTimes;
  llvm::DenseMap<mlir::Value, mlir::Value> signalIds;
  llvm::DenseMap<mlir::Value, bool> iterState;
  llvm::DenseMap<mlir::Value, mlir::Value> iterInitValue;

  llvm::DenseMap<mlir::Value, mlir::Value> valueIds;
  llvm::DenseMap<mlir::Block *, uint64_t> blockExs;
}; // Runner
}

namespace acdc {

void CommandProcessor::run(mlir::ModuleOp module) {

  std::string topLevelFunction("graph");
  mlir::Operation *mainP = module.lookupSymbol(topLevelFunction);
  // The toplevel function can accept any number of operands, and returns
  // any number of results.
  if (!mainP) {
    llvm::errs() << "Toplevel function " << topLevelFunction << " not found!\n";
  }

  // We need three things in a function-type independent way.
  // The type signature of the function.
  mlir::FunctionType ftype;
  // The arguments of the entry block.
  mlir::Block::BlockArgListType blockArgs;


  Runner runner(traceStream);

  // The number of inputs to the function in the IR.
  unsigned numInputs = 0;
  unsigned numOutputs = 0;

  if (mlir::FuncOp toplevel =
      module.lookupSymbol<mlir::FuncOp>(topLevelFunction)) {
    runner.buildIdMap(toplevel);
    runner.buildExMap(toplevel);
    ftype = toplevel.getType();
    mlir::Block &entryBlock = toplevel.getBody().front();
    blockArgs = entryBlock.getArguments();

    // Get the primary inputs of toplevel off the command line.
    numInputs = ftype.getNumInputs();
    numOutputs = ftype.getNumResults();
  } else {
    llvm_unreachable("Function not supported.\n");
  }

  runner.emitTraceStart(traceStream);

  for(unsigned i = 0; i < numInputs; i++) {
    mlir::Type type = ftype.getInput(i);
    if (auto tensorTy = type.dyn_cast<mlir::TensorType>()) {
      // We require this memref type to be fully specified.
      // runner.valueMap[blockArgs[i]]++;
    } else {
      llvm_unreachable("Only memref arguments are supported.\n");
    }
  }

  std::vector<llvm::Any> results(numOutputs);
  std::vector<uint64_t> resultTimes(numOutputs);
  if(mlir::FuncOp toplevel =
     module.lookupSymbol<mlir::FuncOp>(topLevelFunction)) {
    runner.simulateFunction(toplevel);
  }

  #if 0
  // Go back through the arguments and output any memrefs.
  for(unsigned i = 0; i < numInputs; i++) {
    mlir::Type type = ftype.getInput(i);
    if (type.isa<mlir::MemRefType>()) {
      // We require this memref type to be fully specified.
      auto memreftype = type.dyn_cast<mlir::MemRefType>();
      unsigned buffer = llvm::any_cast<unsigned>(valueDefMap[blockArgs[i]]);
      auto elementType = memreftype.getElementType();
      for(int j = 0; j < memreftype.getNumElements(); j++) {
        if(j != 0) llvm::outs() << ",";
        llvm::outs() << printAnyValueWithType(elementType,
                                        store[buffer][j]);
      }
      llvm::outs() << " ";
    }
  }
  #endif

  runner.emitTraceEnd(traceStream);

}// CommandProcessor::run

} // namespace acdc
