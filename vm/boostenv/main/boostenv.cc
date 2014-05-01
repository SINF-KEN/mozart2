// Copyright © 2011, Université catholique de Louvain
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// *  Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// *  Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "boostenv.hh"

#include <fstream>
#include <boost/random/random_device.hpp>

namespace mozart { namespace boostenv {

/////////////
// BoostVM //
/////////////

BoostVM::BoostVM(BoostEnvironment& environment,
                 nativeint identifier,
                 VirtualMachineOptions options,
                 const std::string& app, bool isURL) :
  VirtualMachine(environment, options), vm(this),
  env(environment), identifier(identifier),
  uuidGenerator(random_generator),
  _asyncIONodeCount(0),
  preemptionTimer(environment.io_service),
  alarmTimer(environment.io_service),
  _terminated(false) {

  // Make sure the IO thread will wait for us
  _work = new boost::asio::io_service::work(environment.io_service);

  builtins::biref::registerBuiltinModOS(vm);
  builtins::biref::registerBuiltinModVM(vm);

  // Initialize the pseudo random number generator with a really random seed
  boost::random::random_device generator;
  random_generator.seed(generator);

  UnstableNode future = ReadOnlyVariable::build(vm);
  _stream = _headOfStream = RichNode(future).getStableRef(vm);

  // Finally start the VM thread, which will initialize and run the VM
  _thread = boost::thread(&BoostVM::start, this, app, isURL);
};

void BoostVM::start(std::string app, bool isURL) {
  if (!env.vmStarter(vm, app, isURL))
    std::cerr << "Could not start VM." << std::endl;
}

void BoostVM::run() {
  constexpr auto recNeverInvokeAgain = VirtualMachine::recNeverInvokeAgain;
  constexpr auto recInvokeAgainNow   = VirtualMachine::recInvokeAgainNow;
  constexpr auto recInvokeAgainLater = VirtualMachine::recInvokeAgainLater;

  // The main loop that handles all interactions with the VM
  while (true) {
    // Make sure the VM knows the reference time before starting
    vm->setReferenceTime(env.getReferenceTime());

    // Setup the preemption timer
    preemptionTimer.expires_from_now(boost::posix_time::millisec(1));
    preemptionTimer.async_wait(boost::bind(
      &BoostVM::onPreemptionTimerExpire,
      this, boost::asio::placeholders::error));

    // Run the VM
    auto nextInvokePair = vm->run();
    auto nextInvoke = nextInvokePair.first;

    // Stop the preemption timer
    preemptionTimer.cancel();

    {
      // Acquire the lock that grants me access to
      // _conditionWorkToDoInVM and _vmEventsCallbacks
      boost::unique_lock<boost::mutex> lock(_conditionWorkToDoInVMMutex);

      // Is there anything left to do?
      if (!isRunning() || ((nextInvoke == recNeverInvokeAgain) &&
          (_asyncIONodeCount == 0) && _vmEventsCallbacks.empty())) {
        // Totally finished, nothing can ever wake me again
        break;
      }

      // Handle asynchronous events coming from I/O, e.g.
      while (!_vmEventsCallbacks.empty()) {
        _vmEventsCallbacks.front()();
        _vmEventsCallbacks.pop();

        // That could have created work for the VM
        nextInvoke = recInvokeAgainNow;
      }

      // Unless asked to invoke again now, setup the wait
      if (nextInvoke != recInvokeAgainNow) {
        // Setup the alarm time, if asked by the VM
        if (nextInvoke == recInvokeAgainLater) {
          alarmTimer.expires_at(
            BoostEnvironment::referenceTimeToPTime(nextInvokePair.second));
          alarmTimer.async_wait([this] (const boost::system::error_code& err) {
            if (!err) {
              boost::lock_guard<boost::mutex> lock(_conditionWorkToDoInVMMutex);
              _conditionWorkToDoInVM.notify_all();
            }
          });
        }

        _conditionWorkToDoInVM.wait(lock);
      }
    }

    // Cancel the alarm timer, in case it was not it that woke me
    alarmTimer.cancel();
  }

  terminate();
}

void BoostVM::onPreemptionTimerExpire(const boost::system::error_code& error) {
  if (error != boost::asio::error::operation_aborted) {
    // Preemption
    vm->setReferenceTime(env.getReferenceTime());
    vm->requestPreempt();

    // Reschedule
    preemptionTimer.expires_at(
      preemptionTimer.expires_at() + boost::posix_time::millisec(1));
    preemptionTimer.async_wait(boost::bind(
      &BoostVM::onPreemptionTimerExpire,
      this, boost::asio::placeholders::error));
  }
}

UUID BoostVM::genUUID() {
  boost::uuids::uuid uuid = uuidGenerator();

  std::uint64_t data0 = bytes2uint64(uuid.data);
  std::uint64_t data1 = bytes2uint64(uuid.data+8);

  return UUID(data0, data1);
}

std::uint64_t BoostVM::bytes2uint64(const std::uint8_t* bytes) {
  return
    ((std::uint64_t) bytes[0] << 56) + ((std::uint64_t) bytes[1] << 48) +
    ((std::uint64_t) bytes[2] << 40) + ((std::uint64_t) bytes[3] << 32) +
    ((std::uint64_t) bytes[4] << 24) + ((std::uint64_t) bytes[5] << 16) +
    ((std::uint64_t) bytes[6] << 8) + ((std::uint64_t) bytes[7] << 0);
}

bool BoostVM::streamAsked() {
  return _headOfStream == nullptr;
}

bool BoostVM::portClosed() {
  return _stream == nullptr;
}

void BoostVM::getStream(UnstableNode &stream) {
  if (streamAsked()) {
    raiseError(vm, "VM.stream can only be called once, otherwise it would leak");
  } else {
    stream.copy(vm, *_headOfStream);
    _asyncIONodeCount++; // Wait for the VM stream until closeStream()
    _headOfStream = nullptr;
  }
}

void BoostVM::closeStream() {
  if (streamAsked() && !portClosed()) {
    _asyncIONodeCount--; // We are no more interested in the stream
    UnstableNode nil = buildNil(vm);
    BindableReadOnly(*_stream).bindReadOnly(vm, nil);
    _stream = nullptr;
  }
}

void BoostVM::receiveOnVMPort(UnstableNode value) {
  if (!portClosed())
    sendToReadOnlyStream(vm, _stream, value);
}

void BoostVM::receiveOnVMPort(std::vector<unsigned char>* buffer) {
  if (portClosed()) {
    delete buffer;
    return;
  }

  std::string str(buffer->begin(), buffer->end());
  std::istringstream input(str);
  UnstableNode unpickled = bootUnpickle(vm, input);
  delete buffer;

  sendToReadOnlyStream(vm, _stream, unpickled);
}

bool BoostVM::isRunning() {
  return !_terminated.load(std::memory_order_acquire);
}

void BoostVM::requestTermination() {
  postVMEvent([this] { this->terminate(); });
}

void BoostVM::addMonitor(BoostVM& monitor) {
  boost::lock_guard<std::mutex> lock(_monitorsMutex);
  _monitors.push_back(monitor.vm);
}

void BoostVM::tellMonitors() {
  std::lock_guard<std::mutex> lock(_monitorsMutex);
  nativeint deadVM = this->identifier;
  for (VM vm : _monitors) {
    BoostVM::forVM(vm).postVMEvent([=] () {
      BoostVM::forVM(vm).receiveOnVMPort(buildTuple(vm, "terminated", deadVM));
    });
  }
}

void BoostVM::terminate() {
  if (!_terminated.load(std::memory_order_acquire)) {
    _terminated.store(true, std::memory_order_release);
    closeStream();
    tellMonitors();
    delete _work;
  }
}

//////////////////////
// BoostEnvironment //
//////////////////////

namespace {
  /* TODO It might be worth, someday, to investigate how we can lift this
   * decoding to the Oz level.
   * It should somewhere in Resolve.oz and/or URL.oz.
   * But at the same time, not forgetting that this function implements
   * bootURLLoad (not a hypothetical bootFileLoad)!
   *
   * In fact it is already a duplicate of the logic in OS.oz.
   */

  inline
  char hexDigitToValue(char digit) {
    // Don't care to give meaningful results if the digit is not valid
    if (digit <= '9')
      return digit - '0';
    else if (digit <= 'Z')
      return digit - ('A'-10);
    else
      return digit - ('a'-10);
  }

  inline
  std::string decodeURL(const std::string& encoded) {
    // Fast path when there is nothing to do
    if (encoded.find('%') == std::string::npos)
      return encoded;

    // Relevant reminder: Unicode URLs are UTF-8 encoded then %-escaped

    std::string decoded;
    decoded.reserve(encoded.size());

    for (size_t i = 0; i < encoded.size(); ++i) {
      char c = encoded[i];
      if (c == '%' && (i+2 < encoded.size())) {
        char v1 = hexDigitToValue(encoded[++i]);
        char v2 = hexDigitToValue(encoded[++i]);
        decoded.push_back((v1 << 4) | v2);
      } else {
        decoded.push_back(c);
      }
    }

    return decoded;
  }

  inline
  std::string decodedURLToFilename(const std::string& url) {
    // Not sure this is the right test (why not // ?), but it was so in Mozart 1
    if (url.substr(0, 5) == "file:")
      return url.substr(5);
    else
      return url;
  }

  bool defaultBootLoader(VM vm, const std::string& url, UnstableNode& result) {
    std::string filename = decodedURLToFilename(decodeURL(url));
    std::ifstream input(filename, std::ios::binary);
    if (!input.is_open())
      return false;
    result = bootUnpickle(vm, input);
    return true;
  }
}

BoostEnvironment::BoostEnvironment(const VMStarter& vmStarter,
                                   VirtualMachineOptions options) :
  _nextVMIdentifier(1), _aliveVMs(0),
  _options(options), vmStarter(vmStarter) {
  // Set up a default boot loader
  setBootLoader(&defaultBootLoader);
}

BoostVM& BoostEnvironment::addVM(const std::string& app, bool isURL) {
  std::lock_guard<std::mutex> lock(_vmsMutex);
  vms.emplace_front(*this, _nextVMIdentifier++, _options, app, isURL);
  _aliveVMs++;
  return vms.front();
}

BoostVM& BoostEnvironment::getVM(VM vm, nativeint identifier) {
  {
    std::lock_guard<std::mutex> lock(_vmsMutex);
    for (BoostVM& vm : vms) {
      if (vm.identifier == identifier)
        return vm;
    }
  }
  raiseError(vm, "Invalid VM identifier: ", identifier);
}

UnstableNode BoostEnvironment::listVMs(VM vm) {
  std::lock_guard<std::mutex> lock(_vmsMutex);
  UnstableNode list = buildList(vm);
  for (BoostVM& boostVM : vms) {
    if (boostVM.isRunning())
      list = buildCons(vm, SmallInt::build(vm, boostVM.identifier), list);
  }
  return list;
}

void BoostEnvironment::killVM(VM vm, nativeint exitCode) {
  if (BoostVM::forVM(vm).isRunning()) {
    BoostVM::forVM(vm).requestTermination();
    if (--_aliveVMs == 0) { // killing the last VM
      std::exit(exitCode);
    }
  }
}

void BoostEnvironment::runIO() {
  // This will end when all VMs are done.
  io_service.run();
}

std::shared_ptr<BigIntImplem> BoostEnvironment::newBigIntImplem(VM vm, nativeint value) {
  return BoostBigInt::make_shared_ptr(value);
}

std::shared_ptr<BigIntImplem> BoostEnvironment::newBigIntImplem(VM vm, double value) {
  return BoostBigInt::make_shared_ptr(value);
}

std::shared_ptr<BigIntImplem> BoostEnvironment::newBigIntImplem(VM vm, const std::string& value) {
  return BoostBigInt::make_shared_ptr(value);
}

void BoostEnvironment::sendToVMPort(VM vm, VM to, RichNode value) {
  if (BoostVM::forVM(to).portClosed())
    return;

  UnstableNode picklePack;
  if (!vm->getPropertyRegistry().get(vm, "pickle.pack", picklePack))
    raiseError(vm, "Could not find property pickle.pack");

  UnstableNode vbs;
  ozcalls::ozCall(vm, "mozart::boostenv::BoostEnvironment::sendToVMPort",
    picklePack, value, ozcalls::out(vbs));

  size_t bufSize = ozVBSLengthForBuffer(vm, vbs);
  // allocates the vector in a neutral zone: the heap
  std::vector<unsigned char> *buffer = new std::vector<unsigned char>();
  ozVBSGet(vm, vbs, bufSize, *buffer);

  BoostVM::forVM(to).postVMEvent([=] () {
    BoostVM::forVM(to).receiveOnVMPort(buffer);
  });
}

} }
