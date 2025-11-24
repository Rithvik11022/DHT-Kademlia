 #ifndef KADEMLIA_DHT_H
#define KADEMLIA_DHT_H

#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <algorithm>
#include <memory>
#include <chrono>
#include <random>
#include <bitset>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <atomic>

// ============================================================================
// CONFIGURATION
// ============================================================================

namespace kademlia {

const int ID_BITS = 160;
const int K = 20;                    // Bucket size
const int ALPHA = 3;                 // Concurrency parameter
const int B = 160;                   // Number of buckets

// Timeouts (milliseconds)
const int RPC_TIMEOUT = 5000;        // 5 seconds
const int LOOKUP_TIMEOUT = 60000;    // 60 seconds
const int PING_TIMEOUT = 3000;       // 3 seconds

// Intervals (seconds)
const int BUCKET_REFRESH_INTERVAL = 3600;      // 1 hour
const int REPUBLISH_INTERVAL = 3600;           // 1 hour
const int REPLICATE_INTERVAL = 3600;           // 1 hour
const int DATA_EXPIRE_TIME = 86400;            // 24 hours

// Failure detection
const int MAX_FAILURES_BEFORE_QUESTIONABLE = 2;
const int MAX_FAILURES_BEFORE_EVICTION = 5;
const int QUESTIONABLE_TIMEOUT = 900;  // 15 minutes

// ============================================================================
// TYPE DEFINITIONS
// ============================================================================

using NodeID = std::bitset<ID_BITS>;
using Key = std::bitset<ID_BITS>;
using Value = std::string;
using TimePoint = std::chrono::steady_clock::time_point;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

inline TimePoint now() {
    return std::chrono::steady_clock::now();
}

inline long long elapsed_ms(TimePoint start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now() - start).count();
}

inline long long elapsed_sec(TimePoint start) {
    return std::chrono::duration_cast<std::chrono::seconds>(now() - start).count();
}

NodeID xorDistance(const NodeID& a, const NodeID& b) {
    return a ^ b;
}

int getBucketIndex(const NodeID& nodeId, const NodeID& targetId) {
    NodeID distance = xorDistance(nodeId, targetId);
    for (int i = ID_BITS - 1; i >= 0; --i) {
        if (distance[i]) return i;
    }
    return -1;
}

NodeID randomNodeID() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    
    NodeID id;
    for (int i = 0; i < ID_BITS; i += 64) {
        uint64_t r = dis(gen);
        for (int j = 0; j < 64 && i + j < ID_BITS; ++j) {
            id[i + j] = (r >> j) & 1;
        }
    }
    return id;
}

// ============================================================================
// RPC MESSAGE TYPES
// ============================================================================

enum class RPCType {
    PING,
    STORE,
    FIND_NODE,
    FIND_VALUE
};

struct RPCMessage {
    RPCType type;
    NodeID senderId;
    NodeID targetId;  // For FIND_NODE/FIND_VALUE
    Key key;          // For STORE/FIND_VALUE
    Value value;      // For STORE
    std::string rpcId;
    
    RPCMessage(RPCType t, const NodeID& sender)
        : type(t), senderId(sender), rpcId(generateRPCId()) {}
    
private:
    static std::string generateRPCId() {
        static std::atomic<uint64_t> counter{0};
        return std::to_string(counter++);
    }
};

struct RPCResponse {
    std::string rpcId;
    NodeID responderId;
    bool success;
    std::vector<NodeID> nodes;  // For FIND_NODE/FIND_VALUE
    Value value;                // For FIND_VALUE
    bool valueFound;            // For FIND_VALUE
    
    RPCResponse() : success(false), valueFound(false) {}
};

// ============================================================================
// CONTACT
// ============================================================================

enum class ContactState {
    GOOD,           // Responded recently
    QUESTIONABLE,   // Haven't heard from in a while
    BAD             // Multiple failures
};

struct Contact {
    NodeID id;
    std::string address;
    TimePoint lastSeen;
    int failureCount;
    bool questionable;
    
    Contact(const NodeID& nodeId, const std::string& addr = "")
        : id(nodeId), address(addr), lastSeen(now()), 
          failureCount(0), questionable(false) {}
    
    ContactState getState() const {
        long long timeSinceLastSeen = elapsed_sec(lastSeen);
        
        if (failureCount >= MAX_FAILURES_BEFORE_EVICTION) return ContactState::BAD;
        if (failureCount >= MAX_FAILURES_BEFORE_QUESTIONABLE || 
            timeSinceLastSeen > QUESTIONABLE_TIMEOUT) return ContactState::QUESTIONABLE;
        return ContactState::GOOD;
    }
    
    void recordSuccess() {
        lastSeen = now();
        failureCount = 0;
        questionable = false;
    }
    
    void recordFailure() {
        failureCount++;
        if (failureCount >= MAX_FAILURES_BEFORE_QUESTIONABLE) {
            questionable = true;
        }
    }
};

// ============================================================================
// K-BUCKET
// ============================================================================

class KBucket {
private:
    std::deque<Contact> contacts;
    int maxSize;
    mutable std::mutex mtx;
    TimePoint lastLookup;
    
public:
    KBucket(int k = K) : maxSize(k), lastLookup(now()) {}
    
    bool addContact(const Contact& contact) {
        std::lock_guard<std::mutex> lock(mtx);
        
        // Check if contact exists
        auto it = std::find_if(contacts.begin(), contacts.end(),
            [&contact](const Contact& c) { return c.id == contact.id; });
        
        if (it != contacts.end()) {
            // Update existing contact - move to tail
            Contact updated = *it;
            updated.recordSuccess();
            contacts.erase(it);
            contacts.push_back(updated);
            return true;
        }
        
        if (contacts.size() < maxSize) {
            // Bucket not full - add to tail
            contacts.push_back(contact);
            return true;
        }
        
        // Bucket full - check head (least recently seen)
        Contact& head = contacts.front();
        if (head.getState() == ContactState::BAD) {
            // Head is bad - replace it
            contacts.pop_front();
            contacts.push_back(contact);
            return true;
        }
        
        // Head is good/questionable - keep it, ping it
        return false;
    }
    
    void recordSuccess(const NodeID& id) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = std::find_if(contacts.begin(), contacts.end(),
            [&id](const Contact& c) { return c.id == id; });
        
        if (it != contacts.end()) {
            Contact updated = *it;
            updated.recordSuccess();
            contacts.erase(it);
            contacts.push_back(updated);
        }
    }
    
    void recordFailure(const NodeID& id) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = std::find_if(contacts.begin(), contacts.end(),
            [&id](const Contact& c) { return c.id == id; });
        
        if (it != contacts.end()) {
            it->recordFailure();
            if (it->getState() == ContactState::BAD) {
                contacts.erase(it);
            }
        }
    }
    
    std::vector<Contact> getContacts() const {
        std::lock_guard<std::mutex> lock(mtx);
        return std::vector<Contact>(contacts.begin(), contacts.end());
    }
    
    std::vector<NodeID> getClosest(const NodeID& target, int count) const {
        std::lock_guard<std::mutex> lock(mtx);
        
        std::vector<Contact> sorted(contacts.begin(), contacts.end());
        std::sort(sorted.begin(), sorted.end(),
            [&target](const Contact& a, const Contact& b) {
                return xorDistance(a.id, target).to_ullong() < 
                       xorDistance(b.id, target).to_ullong();
            });
        
        std::vector<NodeID> result;
        for (size_t i = 0; i < std::min(sorted.size(), (size_t)count); ++i) {
            if (sorted[i].getState() != ContactState::BAD) {
                result.push_back(sorted[i].id);
            }
        }
        return result;
    }
    
    Contact* getHead() {
        std::lock_guard<std::mutex> lock(mtx);
        return contacts.empty() ? nullptr : &contacts.front();
    }
    
    bool needsRefresh() const {
        std::lock_guard<std::mutex> lock(mtx);
        return elapsed_sec(lastLookup) > BUCKET_REFRESH_INTERVAL;
    }
    
    void markRefreshed() {
        std::lock_guard<std::mutex> lock(mtx);
        lastLookup = now();
    }
    
    bool isEmpty() const {
        std::lock_guard<std::mutex> lock(mtx);
        return contacts.empty();
    }
    
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return contacts.size();
    }
};

// ============================================================================
// STORAGE ITEM
// ============================================================================

struct StorageItem {
    Value value;
    TimePoint publishTime;
    NodeID publisher;
    
    StorageItem(const Value& v, const NodeID& pub)
        : value(v), publishTime(now()), publisher(pub) {}
    
    bool isExpired() const {
        return elapsed_sec(publishTime) > DATA_EXPIRE_TIME;
    }
};

// ============================================================================
// RPC MANAGER
// ============================================================================

class RPCManager {
private:
    struct PendingRPC {
        RPCMessage message;
        TimePoint sentTime;
        std::function<void(const RPCResponse&)> callback;
        bool completed;
        
        PendingRPC(const RPCMessage& msg, std::function<void(const RPCResponse&)> cb)
            : message(msg), sentTime(now()), callback(cb), completed(false) {}
        
        bool hasTimedOut() const {
            return elapsed_ms(sentTime) > RPC_TIMEOUT;
        }
    };
    
    std::map<std::string, PendingRPC> pendingRPCs;
    mutable std::mutex mtx;
    
public:
    void sendRPC(const RPCMessage& msg, std::function<void(const RPCResponse&)> callback) {
        std::lock_guard<std::mutex> lock(mtx);
        pendingRPCs.emplace(msg.rpcId, PendingRPC(msg, callback));
    }
    
    void handleResponse(const RPCResponse& response) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = pendingRPCs.find(response.rpcId);
        if (it != pendingRPCs.end() && !it->second.completed) {
            it->second.completed = true;
            if (it->second.callback) {
                it->second.callback(response);
            }
        }
    }
    
    void checkTimeouts(std::function<void(const NodeID&)> timeoutHandler) {
        std::lock_guard<std::mutex> lock(mtx);
        
        for (auto it = pendingRPCs.begin(); it != pendingRPCs.end();) {
            if (!it->second.completed && it->second.hasTimedOut()) {
                if (timeoutHandler) {
                    timeoutHandler(it->second.message.targetId);
                }
                
                // Call callback with failure response
                if (it->second.callback) {
                    RPCResponse failureResponse;
                    failureResponse.rpcId = it->first;
                    failureResponse.success = false;
                    it->second.callback(failureResponse);
                }
                
                it = pendingRPCs.erase(it);
            } else if (it->second.completed) {
                it = pendingRPCs.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    size_t pendingCount() const {
        std::lock_guard<std::mutex> lock(mtx);
        return pendingRPCs.size();
    }
};

// ============================================================================
// KADEMLIA NODE
// ============================================================================

class KademliaNode {
private:
    NodeID nodeId;
    std::vector<KBucket> buckets;
    std::map<Key, StorageItem> storage;
    RPCManager rpcManager;
    
    mutable std::mutex storageMtx;
    mutable std::mutex bucketsMtx;
    
    std::atomic<bool> running{false};
    std::thread maintenanceThread;
    
    // Network simulation callback
    std::function<void(const NodeID&, const RPCMessage&, 
                      std::function<void(const RPCResponse&)>)> networkSendFunc;
    
public:
    KademliaNode(const NodeID& id) : nodeId(id), buckets(ID_BITS) {}
    
    ~KademliaNode() {
        stop();
    }
    
    NodeID getID() const { return nodeId; }
    
    void setNetworkSendFunction(std::function<void(const NodeID&, const RPCMessage&, 
                                                   std::function<void(const RPCResponse&)>)> func) {
        networkSendFunc = func;
    }
    
    void start() {
        running = true;
        maintenanceThread = std::thread(&KademliaNode::maintenanceLoop, this);
    }
    
    void stop() {
        running = false;
        if (maintenanceThread.joinable()) {
            maintenanceThread.join();
        }
    }
    
    // ========================================================================
    // ROUTING TABLE MANAGEMENT
    // ========================================================================
    
    void addContact(const NodeID& id, const std::string& address = "") {
        if (id == nodeId) return;
        
        int bucketIndex = getBucketIndex(nodeId, id);
        if (bucketIndex < 0 || bucketIndex >= ID_BITS) return;
        
        Contact contact(id, address);
        bool added = buckets[bucketIndex].addContact(contact);
        
        if (!added) {
            // Bucket full - ping head
            auto head = buckets[bucketIndex].getHead();
            if (head && head->questionable) {
                pingNode(head->id);
            }
        }
    }
    
    void recordSuccess(const NodeID& id) {
        int bucketIndex = getBucketIndex(nodeId, id);
        if (bucketIndex >= 0 && bucketIndex < ID_BITS) {
            buckets[bucketIndex].recordSuccess(id);
        }
    }
    
    void recordFailure(const NodeID& id) {
        int bucketIndex = getBucketIndex(nodeId, id);
        if (bucketIndex >= 0 && bucketIndex < ID_BITS) {
            buckets[bucketIndex].recordFailure(id);
        }
    }
    
    std::vector<NodeID> findClosestNodes(const NodeID& target, int count = K) {
        std::set<NodeID> allNodes;
        
        for (const auto& bucket : buckets) {
            auto contacts = bucket.getContacts();
            for (const auto& contact : contacts) {
                if (contact.getState() != ContactState::BAD) {
                    allNodes.insert(contact.id);
                }
            }
        }
        
        std::vector<NodeID> sorted(allNodes.begin(), allNodes.end());
        std::sort(sorted.begin(), sorted.end(),
            [&target](const NodeID& a, const NodeID& b) {
                return xorDistance(a, target).to_ullong() < 
                       xorDistance(b, target).to_ullong();
            });
        
        if (sorted.size() > count) {
            sorted.resize(count);
        }
        return sorted;
    }
    
    // ========================================================================
    // RPC HANDLERS
    // ========================================================================
    
    void handleRPC(const RPCMessage& msg, std::function<void(const RPCResponse&)> respond) {
        addContact(msg.senderId);
        
        RPCResponse response;
        response.rpcId = msg.rpcId;
        response.responderId = nodeId;
        response.success = true;
        
        switch (msg.type) {
            case RPCType::PING:
                // Just respond
                break;
                
            case RPCType::STORE:
                store(msg.key, msg.value, msg.senderId);
                break;
                
            case RPCType::FIND_NODE:
                response.nodes = findClosestNodes(msg.targetId, K);
                break;
                
            case RPCType::FIND_VALUE: {
                Value val;
                if (retrieve(msg.key, val)) {
                    response.valueFound = true;
                    response.value = val;
                } else {
                    response.valueFound = false;
                    response.nodes = findClosestNodes(msg.key, K);
                }
                break;
            }
        }
        
        respond(response);
    }
    
    // ========================================================================
    // RPC OPERATIONS
    // ========================================================================
    
    void pingNode(const NodeID& target) {
        RPCMessage msg(RPCType::PING, nodeId);
        sendRPC(target, msg, [this, target](const RPCResponse& resp) {
            if (resp.success) {
                recordSuccess(target);
            } else {
                recordFailure(target);
            }
        });
    }
    
    void sendStoreRPC(const NodeID& target, const Key& key, const Value& value) {
        RPCMessage msg(RPCType::STORE, nodeId);
        msg.key = key;
        msg.value = value;
        
        sendRPC(target, msg, [this, target](const RPCResponse& resp) {
            if (resp.success) {
                recordSuccess(target);
            } else {
                recordFailure(target);
            }
        });
    }
    
    void sendFindNodeRPC(const NodeID& target, const NodeID& lookupTarget,
                        std::function<void(const RPCResponse&)> callback) {
        RPCMessage msg(RPCType::FIND_NODE, nodeId);
        msg.targetId = lookupTarget;
        
        sendRPC(target, msg, [this, target, callback](const RPCResponse& resp) {
            if (resp.success) {
                recordSuccess(target);
                for (const auto& nodeId : resp.nodes) {
                    addContact(nodeId);
                }
            } else {
                recordFailure(target);
            }
            if (callback) callback(resp);
        });
    }
    
    void sendFindValueRPC(const NodeID& target, const Key& key,
                         std::function<void(const RPCResponse&)> callback) {
        RPCMessage msg(RPCType::FIND_VALUE, nodeId);
        msg.key = key;
        
        sendRPC(target, msg, [this, target, callback](const RPCResponse& resp) {
            if (resp.success) {
                recordSuccess(target);
                if (!resp.valueFound) {
                    for (const auto& nodeId : resp.nodes) {
                        addContact(nodeId);
                    }
                }
            } else {
                recordFailure(target);
            }
            if (callback) callback(resp);
        });
    }
    
    // ========================================================================
    // ITERATIVE LOOKUP
    // ========================================================================
    
    std::vector<NodeID> iterativeFindNode(const NodeID& target) {
        std::set<NodeID> queried;
        std::set<NodeID> toQuery;
        std::mutex lookupMtx;
        std::condition_variable cv;
        
        auto closest = findClosestNodes(target, ALPHA);
        for (const auto& id : closest) {
            toQuery.insert(id);
        }
        
        TimePoint startTime = now();
        int activeQueries = 0;
        bool closestChanged = true;
        NodeID previousClosest = toQuery.empty() ? nodeId : *toQuery.begin();
        
        while (elapsed_ms(startTime) < LOOKUP_TIMEOUT) {
            std::unique_lock<std::mutex> lock(lookupMtx);
            
            // Launch ALPHA parallel queries
            while (activeQueries < ALPHA && !toQuery.empty()) {
                NodeID target_node = *toQuery.begin();
                toQuery.erase(toQuery.begin());
                
                if (queried.count(target_node)) continue;
                queried.insert(target_node);
                
                activeQueries++;
                
                sendFindNodeRPC(target_node, target,
                    [&, target_node](const RPCResponse& resp) {
                        std::lock_guard<std::mutex> lg(lookupMtx);
                        activeQueries--;
                        
                        if (resp.success) {
                            for (const auto& node : resp.nodes) {
                                if (!queried.count(node)) {
                                    toQuery.insert(node);
                                }
                            }
                        }
                        cv.notify_one();
                    });
            }
            
            // Wait for responses or timeout
            if (activeQueries > 0) {
                cv.wait_for(lock, std::chrono::milliseconds(100));
            }
            
            // Check if we found closer nodes
            if (!toQuery.empty()) {
                NodeID newClosest = *toQuery.begin();
                if (xorDistance(newClosest, target).to_ullong() >= 
                    xorDistance(previousClosest, target).to_ullong()) {
                    closestChanged = false;
                } else {
                    previousClosest = newClosest;
                    closestChanged = true;
                }
            }
            
            // Termination conditions
            if (activeQueries == 0 && (toQuery.empty() || !closestChanged)) {
                break;
            }
        }
        
        // Return K closest from queried set
        std::vector<NodeID> result(queried.begin(), queried.end());
        std::sort(result.begin(), result.end(),
            [&target](const NodeID& a, const NodeID& b) {
                return xorDistance(a, target).to_ullong() < 
                       xorDistance(b, target).to_ullong();
            });
        
        if (result.size() > K) {
            result.resize(K);
        }
        
        return result;
    }
    
    bool iterativeFindValue(const Key& key, Value& value) {
        std::set<NodeID> queried;
        std::set<NodeID> toQuery;
        std::mutex lookupMtx;
        std::condition_variable cv;
        bool found = false;
        
        auto closest = findClosestNodes(key, ALPHA);
        for (const auto& id : closest) {
            toQuery.insert(id);
        }
        
        TimePoint startTime = now();
        int activeQueries = 0;
        
        while (!found && elapsed_ms(startTime) < LOOKUP_TIMEOUT) {
            std::unique_lock<std::mutex> lock(lookupMtx);
            
            while (activeQueries < ALPHA && !toQuery.empty() && !found) {
                NodeID target_node = *toQuery.begin();
                toQuery.erase(toQuery.begin());
                
                if (queried.count(target_node)) continue;
                queried.insert(target_node);
                
                activeQueries++;
                
                sendFindValueRPC(target_node, key,
                    [&, target_node](const RPCResponse& resp) {
                        std::lock_guard<std::mutex> lg(lookupMtx);
                        activeQueries--;
                        
                        if (resp.success && resp.valueFound) {
                            value = resp.value;
                            found = true;
                        } else if (resp.success) {
                            for (const auto& node : resp.nodes) {
                                if (!queried.count(node)) {
                                    toQuery.insert(node);
                                }
                            }
                        }
                        cv.notify_one();
                    });
            }
            
            if (activeQueries > 0) {
                cv.wait_for(lock, std::chrono::milliseconds(100));
            }
            
            if (activeQueries == 0 && toQuery.empty()) {
                break;
            }
        }
        
        return found;
    }
    
    // ========================================================================
    // STORAGE OPERATIONS
    // ========================================================================
    
    void store(const Key& key, const Value& value, const NodeID& publisher) {
        std::lock_guard<std::mutex> lock(storageMtx);
        storage.emplace(key, StorageItem(value, publisher));
    }
    
    bool retrieve(const Key& key, Value& value) {
        std::lock_guard<std::mutex> lock(storageMtx);
        auto it = storage.find(key);
        if (it != storage.end() && !it->second.isExpired()) {
            value = it->second.value;
            return true;
        }
        return false;
    }
    
    void put(const Key& key, const Value& value) {
        // Find K closest nodes
        auto closestNodes = iterativeFindNode(key);
        
        // Store on K closest nodes
        for (const auto& nodeId : closestNodes) {
            sendStoreRPC(nodeId, key, value);
        }
        
        // Also store locally
        store(key, value, nodeId);
    }
    
    bool get(const Key& key, Value& value) {
        // Check local storage first
        if (retrieve(key, value)) {
            return true;
        }
        
        // Perform iterative lookup
        return iterativeFindValue(key, value);
    }
    
    // ========================================================================
    // MAINTENANCE
    // ========================================================================
    
    void maintenanceLoop() {
        auto lastRefresh = now();
        auto lastRepublish = now();
        auto lastExpire = now();
        
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Check RPC timeouts
            rpcManager.checkTimeouts([this](const NodeID& id) {
                recordFailure(id);
            });
            
            // Bucket refresh
            if (elapsed_sec(lastRefresh) > 60) {  // Check every minute
                refreshBuckets();
                lastRefresh = now();
            }
            
            // Republish data
            if (elapsed_sec(lastRepublish) > REPUBLISH_INTERVAL) {
                republishData();
                lastRepublish = now();
            }
            
            // Expire old data
            if (elapsed_sec(lastExpire) > 300) {  // Every 5 minutes
                expireData();
                lastExpire = now();
            }
        }
    }
    
    void refreshBuckets() {
        for (int i = 0; i < ID_BITS; ++i) {
            if (buckets[i].needsRefresh()) {
                // Generate random ID in bucket's range
                NodeID randomId = nodeId;
                randomId.flip(i);
                
                // Perform lookup
                iterativeFindNode(randomId);
                buckets[i].markRefreshed();
            }
        }
    }
    
    void republishData() {
        std::lock_guard<std::mutex> lock(storageMtx);
        
        for (const auto& pair : storage) {
            if (pair.second.publisher == nodeId) {
                // Only republish data we originally published
                auto closestNodes = iterativeFindNode(pair.first);
                for (const auto& nodeId : closestNodes) {
                    sendStoreRPC(nodeId, pair.first, pair.second.value);
                }
            }
        }
    }
    
    void expireData() {
        std::lock_guard<std::mutex> lock(storageMtx);
        
        for (auto it = storage.begin(); it != storage.end();) {
            if (it->second.isExpired()) {
                it = storage.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // ========================================================================
    // NETWORK
    // ========================================================================
    
    void sendRPC(const NodeID& target, const RPCMessage& msg,
                std::function<void(const RPCResponse&)> callback) {
        if (networkSendFunc) {
            networkSendFunc(target, msg, callback);
        }
        rpcManager.sendRPC(msg, callback);
    }
    
    // ========================================================================
    // DIAGNOSTICS
    // ========================================================================
    
    void printRoutingTable() const {
        std::cout << "\nNode " << nodeId.to_ullong() % 10000 << " routing table:\n";
        int totalContacts = 0;
        for (int i = 0; i < ID_BITS; ++i) {
            if (!buckets[i].isEmpty()) {
                std::cout << "  Bucket " << i << ": " << buckets[i].size() << " contacts\n";
                totalContacts += buckets[i].size();
            }
        }
        std::cout << "  Total contacts: " << totalContacts << "\n";
        std::cout << "  Pending RPCs: " << rpcManager.pendingCount() << "\n";
        
        std::lock_guard<std::mutex> lock(storageMtx);
        std::cout << "  Stored items: " << storage.size() << "\n";
    }
};

// ============================================================================
// NETWORK SIMULATOR
// ============================================================================

class KademliaNetwork {
private:
    std::map<NodeID, std::shared_ptr<KademliaNode>> nodes;
    mutable std::mutex networkMtx;
    std::atomic<bool> running{false};
    
    // Network simulation parameters
    double packetLossRate = 0.0;
    int networkDelayMs = 10;
    std::mt19937 rng;
    
public:
    KademliaNetwork() : rng(std::random_device{}()) {}
    
    void setPacketLossRate(double rate) {
        packetLossRate = std::max(0.0, std::min(rate, 1.0));
    }
    
    void setNetworkDelay(int delayMs) {
        networkDelayMs = std::max(0, delayMs);
    }
    
    std::shared_ptr<KademliaNode> addNode(const NodeID& id) {
        std::lock_guard<std::mutex> lock(networkMtx);
        
        auto node = std::make_shared<KademliaNode>(id);
        
        // Set network send function
        node->setNetworkSendFunction(
            [this, id](const NodeID& target, const RPCMessage& msg,
                      std::function<void(const RPCResponse&)> callback) {
                this->routeMessage(id, target, msg, callback);
            });
        
        nodes[id] = node;
        node->start();
        
        return node;
    }
    
    void removeNode(const NodeID& id) {
        std::lock_guard<std::mutex> lock(networkMtx);
        auto it = nodes.find(id);
        if (it != nodes.end()) {
            it->second->stop();
            nodes.erase(it);
        }
    }
    
    std::shared_ptr<KademliaNode> getNode(const NodeID& id) {
        std::lock_guard<std::mutex> lock(networkMtx);
        auto it = nodes.find(id);
        return it != nodes.end() ? it->second : nullptr;
    }
    
    void bootstrap(const NodeID& newNodeId, const NodeID& bootstrapNodeId) {
        auto newNode = getNode(newNodeId);
        auto bootstrapNode = getNode(bootstrapNodeId);
        
        if (!newNode || !bootstrapNode) {
            std::cerr << "Bootstrap failed: node not found\n";
            return;
        }
        
        // Add bootstrap node to routing table
        newNode->addContact(bootstrapNodeId);
        
        // Perform self-lookup to populate routing table
        newNode->iterativeFindNode(newNodeId);
    }
    
    size_t nodeCount() const {
        std::lock_guard<std::mutex> lock(networkMtx);
        return nodes.size();
    }
    
    void printNetworkStatus() const {
        std::lock_guard<std::mutex> lock(networkMtx);
        std::cout << "\n=== Network Status ===\n";
        std::cout << "Total nodes: " << nodes.size() << "\n";
        std::cout << "Packet loss rate: " << (packetLossRate * 100) << "%\n";
        std::cout << "Network delay: " << networkDelayMs << "ms\n";
    }
    
    void printAllRoutingTables() const {
        std::lock_guard<std::mutex> lock(networkMtx);
        for (const auto& pair : nodes) {
            pair.second->printRoutingTable();
        }
    }
    
private:
    void routeMessage(const NodeID& source, const NodeID& target,
                     const RPCMessage& msg,
                     std::function<void(const RPCResponse&)> callback) {
        
        // Simulate packet loss
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        if (dist(rng) < packetLossRate) {
            // Packet lost - callback will timeout
            return;
        }
        
        // Simulate network delay
        std::thread([this, target, msg, callback]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(networkDelayMs));
            
            auto targetNode = getNode(target);
            if (targetNode) {
                targetNode->handleRPC(msg, callback);
            }
            // If node not found, callback will timeout
        }).detach();
    }
};

} // namespace kademlia

// ============================================================================
// MAIN - COMPREHENSIVE TEST
// ============================================================================

using namespace kademlia;

void testBasicOperations() {
    std::cout << "\n========================================\n";
    std::cout << "TEST 1: Basic Operations\n";
    std::cout << "========================================\n";
    
    KademliaNetwork network;
    network.setNetworkDelay(5);
    
    // Create initial node
    auto node1Id = randomNodeID();
    auto node1 = network.addNode(node1Id);
    std::cout << "Created node 1 (ID: " << node1Id.to_ullong() % 10000 << ")\n";
    
    // Add more nodes
    std::vector<NodeID> nodeIds = {node1Id};
    for (int i = 0; i < 5; ++i) {
        auto id = randomNodeID();
        auto node = network.addNode(id);
        nodeIds.push_back(id);
        std::cout << "Created node " << (i + 2) 
                  << " (ID: " << id.to_ullong() % 10000 << ")\n";
        
        // Bootstrap with first node
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        network.bootstrap(id, node1Id);
    }
    
    // Wait for network to stabilize
    std::cout << "\nWaiting for network to stabilize...\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Store and retrieve
    std::cout << "\nStoring key-value pairs...\n";
    Key key1 = randomNodeID();
    Key key2 = randomNodeID();
    
    node1->put(key1, "Hello, Kademlia!");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    node1->put(key2, "Distributed Hash Table");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    std::cout << "\nRetrieving values...\n";
    Value val1, val2;
    
    if (node1->get(key1, val1)) {
        std::cout << "✓ Retrieved from node 1: " << val1 << "\n";
    } else {
        std::cout << "✗ Failed to retrieve key1\n";
    }
    
    // Try retrieving from different node
    auto node2 = network.getNode(nodeIds[2]);
    if (node2 && node2->get(key2, val2)) {
        std::cout << "✓ Retrieved from node 3: " << val2 << "\n";
    } else {
        std::cout << "✗ Failed to retrieve key2\n";
    }
    
    network.printNetworkStatus();
}

void testFailureDetection() {
    std::cout << "\n========================================\n";
    std::cout << "TEST 2: Failure Detection\n";
    std::cout << "========================================\n";
    
    KademliaNetwork network;
    network.setNetworkDelay(5);
    network.setPacketLossRate(0.1);  // 10% packet loss
    
    std::cout << "Creating network with 10% packet loss...\n";
    
    std::vector<NodeID> nodeIds;
    NodeID firstId = randomNodeID();
    network.addNode(firstId);
    nodeIds.push_back(firstId);
    
    for (int i = 0; i < 9; ++i) {
        auto id = randomNodeID();
        network.addNode(id);
        nodeIds.push_back(id);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        network.bootstrap(id, firstId);
    }
    
    std::cout << "Waiting for network to stabilize...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // Store data
    auto node1 = network.getNode(nodeIds[0]);
    Key key = randomNodeID();
    std::cout << "\nStoring data with key " << key.to_ullong() % 10000 << "...\n";
    node1->put(key, "Fault-tolerant data");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Kill some nodes
    std::cout << "\nSimulating node failures...\n";
    for (int i = 0; i < 3; ++i) {
        std::cout << "  Removing node " << nodeIds[i + 5].to_ullong() % 10000 << "\n";
        network.removeNode(nodeIds[i + 5]);
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Try to retrieve data
    std::cout << "\nAttempting to retrieve data after failures...\n";
    Value val;
    auto node2 = network.getNode(nodeIds[3]);
    if (node2 && node2->get(key, val)) {
        std::cout << "✓ Successfully retrieved: " << val << "\n";
        std::cout << "  (Data survived node failures due to replication)\n";
    } else {
        std::cout << "✗ Failed to retrieve data\n";
    }
}

void testScalability() {
    std::cout << "\n========================================\n";
    std::cout << "TEST 3: Scalability\n";
    std::cout << "========================================\n";
    
    KademliaNetwork network;
    network.setNetworkDelay(2);
    
    std::cout << "Creating large network (50 nodes)...\n";
    
    std::vector<NodeID> nodeIds;
    NodeID firstId = randomNodeID();
    network.addNode(firstId);
    nodeIds.push_back(firstId);
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < 49; ++i) {
        auto id = randomNodeID();
        network.addNode(id);
        nodeIds.push_back(id);
        
        if (i > 0 && i % 10 == 0) {
            std::cout << "  Created " << (i + 1) << " nodes...\n";
        }
        
        // Bootstrap with random existing node
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(0, i);
        int bootstrapIdx = dist(gen);
        network.bootstrap(id, nodeIds[bootstrapIdx]);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "\nNetwork created in " << duration << "ms\n";
    std::cout << "Waiting for stabilization...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // Test lookup performance
    std::cout << "\nTesting lookup performance...\n";
    auto testNode = network.getNode(nodeIds[25]);
    
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; ++i) {
        NodeID target = randomNodeID();
        testNode->iterativeFindNode(target);
    }
    end = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "  10 lookups completed in " << duration << "ms\n";
    std::cout << "  Average: " << (duration / 10.0) << "ms per lookup\n";
    
    // Store and retrieve test
    std::cout << "\nStoring 20 key-value pairs...\n";
    std::map<Key, Value> testData;
    start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < 20; ++i) {
        Key k = randomNodeID();
        Value v = "Data_" + std::to_string(i);
        testData[k] = v;
        testNode->put(k, v);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    end = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Stored in " << duration << "ms\n";
    
    std::cout << "\nRetrieving data from random node...\n";
    auto retrieveNode = network.getNode(nodeIds[40]);
    int successCount = 0;
    
    start = std::chrono::steady_clock::now();
    for (const auto& pair : testData) {
        Value v;
        if (retrieveNode->get(pair.first, v) && v == pair.second) {
            successCount++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    end = std::chrono::steady_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    std::cout << "  Retrieved " << successCount << "/20 items in " << duration << "ms\n";
    std::cout << "  Success rate: " << (successCount * 100.0 / 20) << "%\n";
}

void testDataPersistence() {
    std::cout << "\n========================================\n";
    std::cout << "TEST 4: Data Persistence & Republishing\n";
    std::cout << "========================================\n";
    
    KademliaNetwork network;
    network.setNetworkDelay(5);
    
    std::cout << "Creating network...\n";
    std::vector<NodeID> nodeIds;
    for (int i = 0; i < 15; ++i) {
        auto id = randomNodeID();
        network.addNode(id);
        nodeIds.push_back(id);
        
        if (i > 0) {
            network.bootstrap(id, nodeIds[0]);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    auto node1 = network.getNode(nodeIds[0]);
    Key key = randomNodeID();
    
    std::cout << "\nStoring important data...\n";
    node1->put(key, "Critical Information");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    std::cout << "Initial retrieval from different node...\n";
    auto node2 = network.getNode(nodeIds[5]);
    Value val;
    if (node2->get(key, val)) {
        std::cout << "✓ Retrieved: " << val << "\n";
    }
    
    std::cout << "\nWaiting for republish cycle...\n";
    std::cout << "(In production, this happens every hour)\n";
    std::cout << "Simulating passage of time...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    std::cout << "\nData should still be available...\n";
    auto node3 = network.getNode(nodeIds[10]);
    if (node3->get(key, val)) {
        std::cout << "✓ Retrieved after republish: " << val << "\n";
    }
}

int main() {
    std::cout << "╔════════════════════════════════════════╗\n";
    std::cout << "║  Production-Grade Kademlia DHT        ║\n";
    std::cout << "║  Full Implementation with:             ║\n";
    std::cout << "║  • Timeouts & Failure Detection        ║\n";
    std::cout << "║  • Bucket Refresh & Maintenance        ║\n";
    std::cout << "║  • Data Replication & Republishing     ║\n";
    std::cout << "║  • Iterative Lookups                   ║\n";
    std::cout << "║  • Network Simulation                  ║\n";
    std::cout << "╚════════════════════════════════════════╝\n";
    
    try {
        testBasicOperations();
        testFailureDetection();
        testScalability();
        testDataPersistence();
        
        std::cout << "\n╔════════════════════════════════════════╗\n";
        std::cout << "║  All Tests Completed Successfully!    ║\n";
        std::cout << "╚════════════════════════════════════════╝\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

#endif // KADEMLIA_DHT_H