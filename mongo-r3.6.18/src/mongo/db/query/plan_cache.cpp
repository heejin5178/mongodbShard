
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_cache.h"

#include <algorithm>
#include <math.h>
#include <memory>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/dbclientinterface.h"  // For QueryOption_foobar
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {

Counter64 PlanCacheEntry::planCacheTotalSizeEstimateBytes;
namespace {

ServerStatusMetricField<Counter64> totalPlanCacheSizeEstimateBytesMetric(
    "query.planCacheTotalSizeEstimateBytes", &PlanCacheEntry::planCacheTotalSizeEstimateBytes);

// Delimiters for cache key encoding.
const char kEncodeDiscriminatorsBegin = '<';
const char kEncodeDiscriminatorsEnd = '>';
const char kEncodeChildrenBegin = '[';
const char kEncodeChildrenEnd = ']';
const char kEncodeChildrenSeparator = ',';
const char kEncodeSortSection = '~';
const char kEncodeProjectionSection = '|';
const char kEncodeCollationSection = '#';

/**
 * Encode user-provided string. Cache key delimiters seen in the
 * user string are escaped with a backslash.
 */
void encodeUserString(StringData s, StringBuilder* keyBuilder) {
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        switch (c) {
            case kEncodeDiscriminatorsBegin:
            case kEncodeDiscriminatorsEnd:
            case kEncodeChildrenBegin:
            case kEncodeChildrenEnd:
            case kEncodeChildrenSeparator:
            case kEncodeSortSection:
            case kEncodeProjectionSection:
            case kEncodeCollationSection:
            case '\\':
                *keyBuilder << '\\';
            // Fall through to default case.
            default:
                *keyBuilder << c;
        }
    }
}

/**
 * String encoding of MatchExpression::MatchType.
 */
const char* encodeMatchType(MatchExpression::MatchType mt) {
    switch (mt) {
        case MatchExpression::AND:
            return "an";

        case MatchExpression::OR:
            return "or";

        case MatchExpression::NOR:
            return "nr";

        case MatchExpression::NOT:
            return "nt";

        case MatchExpression::ELEM_MATCH_OBJECT:
            return "eo";

        case MatchExpression::ELEM_MATCH_VALUE:
            return "ev";

        case MatchExpression::SIZE:
            return "sz";

        case MatchExpression::LTE:
            return "le";

        case MatchExpression::LT:
            return "lt";

        case MatchExpression::EQ:
            return "eq";

        case MatchExpression::GT:
            return "gt";

        case MatchExpression::GTE:
            return "ge";

        case MatchExpression::REGEX:
            return "re";

        case MatchExpression::MOD:
            return "mo";

        case MatchExpression::EXISTS:
            return "ex";

        case MatchExpression::MATCH_IN:
            return "in";

        case MatchExpression::TYPE_OPERATOR:
            return "ty";

        case MatchExpression::GEO:
            return "go";

        case MatchExpression::WHERE:
            return "wh";

        case MatchExpression::ALWAYS_FALSE:
            return "af";

        case MatchExpression::ALWAYS_TRUE:
            return "at";

        case MatchExpression::GEO_NEAR:
            return "gn";

        case MatchExpression::TEXT:
            return "te";

        case MatchExpression::BITS_ALL_SET:
            return "ls";

        case MatchExpression::BITS_ALL_CLEAR:
            return "lc";

        case MatchExpression::BITS_ANY_SET:
            return "ys";

        case MatchExpression::BITS_ANY_CLEAR:
            return "yc";

        case MatchExpression::EXPRESSION:
            return "xp";

        case MatchExpression::INTERNAL_EXPR_EQ:
            return "ee";

        case MatchExpression::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX:
            return "internalSchemaAllElemMatchFromIndex";

        case MatchExpression::INTERNAL_SCHEMA_ALLOWED_PROPERTIES:
            return "internalSchemaAllowedProperties";

        case MatchExpression::INTERNAL_SCHEMA_COND:
            return "internalSchemaCond";

        case MatchExpression::INTERNAL_SCHEMA_EQ:
            return "internalSchemaEq";

        case MatchExpression::INTERNAL_SCHEMA_FMOD:
            return "internalSchemaFmod";

        case MatchExpression::INTERNAL_SCHEMA_MIN_ITEMS:
            return "internalSchemaMinItems";

        case MatchExpression::INTERNAL_SCHEMA_MAX_ITEMS:
            return "internalSchemaMaxItems";

        case MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS:
            return "internalSchemaUniqueItems";

        case MatchExpression::INTERNAL_SCHEMA_XOR:
            return "internalSchemaXor";

        case MatchExpression::INTERNAL_SCHEMA_OBJECT_MATCH:
            return "internalSchemaObjectMatch";

        case MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ:
            return "internalSchemaRootDocEq";

        case MatchExpression::INTERNAL_SCHEMA_MIN_LENGTH:
            return "internalSchemaMinLength";

        case MatchExpression::INTERNAL_SCHEMA_MAX_LENGTH:
            return "internalSchemaMaxLength";

        case MatchExpression::INTERNAL_SCHEMA_MIN_PROPERTIES:
            return "internalSchemaMinProperties";

        case MatchExpression::INTERNAL_SCHEMA_MAX_PROPERTIES:
            return "internalSchemaMaxProperties";

        case MatchExpression::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX:
            return "internalSchemaMatchArrayIndex";

        case MatchExpression::INTERNAL_SCHEMA_TYPE:
            return "internalSchemaType";

        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Encodes GEO match expression.
 * Encoding includes:
 * - type of geo query (within/intersect/near)
 * - geometry type
 * - CRS (flat or spherical)
 */
void encodeGeoMatchExpression(const GeoMatchExpression* tree, StringBuilder* keyBuilder) {
    const GeoExpression& geoQuery = tree->getGeoExpression();

    // Type of geo query.
    switch (geoQuery.getPred()) {
        case GeoExpression::WITHIN:
            *keyBuilder << "wi";
            break;
        case GeoExpression::INTERSECT:
            *keyBuilder << "in";
            break;
        case GeoExpression::INVALID:
            *keyBuilder << "id";
            break;
    }

    // Geometry type.
    // Only one of the shared_ptrs in GeoContainer may be non-NULL.
    *keyBuilder << geoQuery.getGeometry().getDebugType();

    // CRS (flat or spherical)
    if (FLAT == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "fl";
    } else if (SPHERE == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "sp";
    } else if (STRICT_SPHERE == geoQuery.getGeometry().getNativeCRS()) {
        *keyBuilder << "ss";
    } else {
        error() << "unknown CRS type " << (int)geoQuery.getGeometry().getNativeCRS()
                << " in geometry of type " << geoQuery.getGeometry().getDebugType();
        invariant(false);
    }
}

/**
 * Encodes GEO_NEAR match expression.
 * Encode:
 * - isNearSphere
 * - CRS (flat or spherical)
 */
void encodeGeoNearMatchExpression(const GeoNearMatchExpression* tree, StringBuilder* keyBuilder) {
    const GeoNearExpression& nearQuery = tree->getData();

    // isNearSphere
    *keyBuilder << (nearQuery.isNearSphere ? "ns" : "nr");

    // CRS (flat or spherical or strict-winding spherical)
    switch (nearQuery.centroid->crs) {
        case FLAT:
            *keyBuilder << "fl";
            break;
        case SPHERE:
            *keyBuilder << "sp";
            break;
        case STRICT_SPHERE:
            *keyBuilder << "ss";
            break;
        case UNSET:
            error() << "unknown CRS type " << (int)nearQuery.centroid->crs
                    << " in point geometry for near query";
            invariant(false);
            break;
    }
}

}  // namespace

//
// Cache-related functions for CanonicalQuery
//

bool PlanCache::shouldCacheQuery(const CanonicalQuery& query) {
    const QueryRequest& qr = query.getQueryRequest();
    const MatchExpression* expr = query.root();

    // Collection scan
    // No sort order requested
    if (qr.getSort().isEmpty() && expr->matchType() == MatchExpression::AND &&
        expr->numChildren() == 0) {
        return false;
    }

    // Hint provided
    if (!qr.getHint().isEmpty()) {
        return false;
    }

    // Min provided
    // Min queries are a special case of hinted queries.
    if (!qr.getMin().isEmpty()) {
        return false;
    }

    // Max provided
    // Similar to min, max queries are a special case of hinted queries.
    if (!qr.getMax().isEmpty()) {
        return false;
    }

    // We don't read or write from the plan cache for explain. This ensures
    // that explain queries don't affect cache state, and it also makes
    // sure that we can always generate information regarding rejected plans
    // and/or trial period execution of candidate plans.
    if (qr.isExplain()) {
        return false;
    }

    // Tailable cursors won't get cached, just turn into collscans.
    if (query.getQueryRequest().isTailable()) {
        return false;
    }

    // Snapshot is really a hint.
    if (query.getQueryRequest().isSnapshot()) {
        return false;
    }

    return true;
}

//
// CachedSolution
//

CachedSolution::CachedSolution(const PlanCacheKey& key, const PlanCacheEntry& entry)
    : plannerData(entry.plannerData.size()),
      key(key),
      query(entry.query.getOwned()),
      sort(entry.sort.getOwned()),
      projection(entry.projection.getOwned()),
      collation(entry.collation.getOwned()),
      decisionWorks(entry.decision->stats[0]->common.works) {
    // CachedSolution should not having any references into
    // cache entry. All relevant data should be cloned/copied.
    for (size_t i = 0; i < entry.plannerData.size(); ++i) {
        verify(entry.plannerData[i]);
        plannerData[i] = entry.plannerData[i]->clone();
    }
}

CachedSolution::~CachedSolution() {
    for (std::vector<SolutionCacheData*>::const_iterator i = plannerData.begin();
         i != plannerData.end();
         ++i) {
        SolutionCacheData* scd = *i;
        delete scd;
    }
}

//
// PlanCacheEntry
//

std::unique_ptr<PlanCacheEntry> PlanCacheEntry::create(
    const std::vector<QuerySolution*>& solutions,
    std::unique_ptr<const PlanRankingDecision> decision,
    const CanonicalQuery& query,
    Date_t timeOfCreation) {
    invariant(decision);

    // The caller of this constructor is responsible for ensuring
    // that the QuerySolution 's' has valid cacheData. If there's no
    // data to cache you shouldn't be trying to construct a PlanCacheEntry.

    // Copy the solution's cache data into the plan cache entry.
    std::vector<std::unique_ptr<const SolutionCacheData>> solutionCacheData(solutions.size());
    for (size_t i = 0; i < solutions.size(); ++i) {
        invariant(solutions[i]->cacheData.get());
        solutionCacheData[i] =
            std::unique_ptr<const SolutionCacheData>(solutions[i]->cacheData->clone());
    }

    const QueryRequest& qr = query.getQueryRequest();
    BSONObjBuilder projBuilder;
    for (auto elem : qr.getProj()) {
        if (elem.fieldName()[0] == '$') {
            continue;
        }
        projBuilder.append(elem);
    }

    return std::unique_ptr<PlanCacheEntry>(new PlanCacheEntry(
        std::move(solutionCacheData),
        qr.getFilter(),
        qr.getSort(),
        projBuilder.obj(),
        query.getCollator() ? query.getCollator()->getSpec().toBSON() : BSONObj(),
        timeOfCreation,
        std::move(decision),
        {}));
}

PlanCacheEntry::PlanCacheEntry(std::vector<std::unique_ptr<const SolutionCacheData>> plannerData,
                               const BSONObj& query,
                               const BSONObj& sort,
                               const BSONObj& projection,
                               const BSONObj& collation,
                               const Date_t timeOfCreation,
                               std::unique_ptr<const PlanRankingDecision> decision,
                               std::vector<PlanCacheEntryFeedback*> feedback)
    : plannerData(std::move(plannerData)),
      query(query),
      sort(sort),
      projection(projection),
      collation(collation),
      timeOfCreation(timeOfCreation),
      decision(std::move(decision)),
      feedback(std::move(feedback)),
      _entireObjectSize(_estimateObjectSizeInBytes()) {
    // Account for the object in the global metric for estimating the server's total plan cache
    // memory consumption.
    planCacheTotalSizeEstimateBytes.increment(_entireObjectSize);
}

PlanCacheEntry::~PlanCacheEntry() {
    for (size_t i = 0; i < feedback.size(); ++i) {
        delete feedback[i];
    }
    planCacheTotalSizeEstimateBytes.decrement(_entireObjectSize);
}

PlanCacheEntry* PlanCacheEntry::clone() const {
    std::vector<std::unique_ptr<const SolutionCacheData>> solutionCacheData(plannerData.size());
    for (size_t i = 0; i < plannerData.size(); ++i) {
        invariant(plannerData[i]);
        solutionCacheData[i] = std::unique_ptr<const SolutionCacheData>(plannerData[i]->clone());
    }
    auto decisionPtr = std::unique_ptr<PlanRankingDecision>(decision->clone());
    PlanCacheEntry* entry = new PlanCacheEntry(std::move(solutionCacheData),
                                               query,
                                               sort,
                                               projection,
                                               collation,
                                               timeOfCreation,
                                               std::move(decisionPtr),
                                               {});

    // Copy performance stats.
    for (size_t i = 0; i < feedback.size(); ++i) {
        PlanCacheEntryFeedback* fb = new PlanCacheEntryFeedback();
        fb->stats.reset(feedback[i]->stats->clone());
        fb->score = feedback[i]->score;
        entry->feedback.push_back(fb);
    }
    return entry;
}

uint64_t PlanCacheEntry::_estimateObjectSizeInBytes() const {
    return  // Add the size of each entry in 'plannerData' vector.
        container_size_helper::estimateObjectSizeInBytes(
            plannerData,
            [](const auto& cacheData) { return cacheData->estimateObjectSizeInBytes(); },
            true) +
        // Add the size of each entry in 'feedback' vector.
        container_size_helper::estimateObjectSizeInBytes(
            feedback,
            [](const auto& feedbackEntry) { return feedbackEntry->estimateObjectSizeInBytes(); },
            true) +
        // Add the entire size of 'decision' object.
        (decision ? decision->estimateObjectSizeInBytes() : 0) +
        // Add the size of all the owned BSON objects.
        query.objsize() + sort.objsize() + projection.objsize() + collation.objsize() +
        // Add size of the object.
        sizeof(*this);
}

std::string PlanCacheEntry::toString() const {
    return str::stream() << "(query: " << query.toString() << ";sort: " << sort.toString()
                         << ";projection: " << projection.toString()
                         << ";collation: " << collation.toString()
                         << ";solutions: " << plannerData.size()
                         << ";timeOfCreation: " << timeOfCreation.toString() << ")";
}

std::string CachedSolution::toString() const {
    return str::stream() << "key: " << key << '\n';
}

//
// PlanCacheIndexTree
//

void PlanCacheIndexTree::setIndexEntry(const IndexEntry& ie) {
    entry.reset(new IndexEntry(ie));
}

PlanCacheIndexTree* PlanCacheIndexTree::clone() const {
    PlanCacheIndexTree* root = new PlanCacheIndexTree();
    if (NULL != entry.get()) {
        root->index_pos = index_pos;
        root->setIndexEntry(*entry.get());
        root->canCombineBounds = canCombineBounds;
    }
    root->orPushdowns = orPushdowns;

    for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
         it != children.end();
         ++it) {
        PlanCacheIndexTree* clonedChild = (*it)->clone();
        root->children.push_back(clonedChild);
    }
    return root;
}

std::string PlanCacheIndexTree::toString(int indents) const {
    StringBuilder result;
    if (!children.empty()) {
        result << std::string(3 * indents, '-') << "Node\n";
        int newIndent = indents + 1;
        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
             it != children.end();
             ++it) {
            result << (*it)->toString(newIndent);
        }
        return result.str();
    } else {
        result << std::string(3 * indents, '-') << "Leaf ";
        if (NULL != entry.get()) {
            result << entry->name << ", pos: " << index_pos << ", can combine? "
                   << canCombineBounds;
        }
        for (const auto& orPushdown : orPushdowns) {
            result << "Move to ";
            bool firstPosition = true;
            for (auto position : orPushdown.route) {
                if (!firstPosition) {
                    result << ",";
                }
                firstPosition = false;
                result << position;
            }
            result << ": " << orPushdown.indexName << ", pos: " << orPushdown.position
                   << ", can combine? " << orPushdown.canCombineBounds << ". ";
        }
        result << '\n';
    }
    return result.str();
}

//
// SolutionCacheData
//

SolutionCacheData* SolutionCacheData::clone() const {
    SolutionCacheData* other = new SolutionCacheData();
    if (NULL != this->tree.get()) {
        // 'tree' could be NULL if the cached solution
        // is a collection scan.
        other->tree.reset(this->tree->clone());
    }
    other->solnType = this->solnType;
    other->wholeIXSolnDir = this->wholeIXSolnDir;
    other->indexFilterApplied = this->indexFilterApplied;
    return other;
}

std::string SolutionCacheData::toString() const {
    switch (this->solnType) {
        case WHOLE_IXSCAN_SOLN:
            verify(this->tree.get());
            return str::stream() << "(whole index scan solution: "
                                 << "dir=" << this->wholeIXSolnDir << "; "
                                 << "tree=" << this->tree->toString() << ")";
        case COLLSCAN_SOLN:
            return "(collection scan)";
        case USE_INDEX_TAGS_SOLN:
            verify(this->tree.get());
            return str::stream() << "(index-tagged expression tree: "
                                 << "tree=" << this->tree->toString() << ")";
    }
    MONGO_UNREACHABLE;
}

//
// PlanCache
//

PlanCache::PlanCache() : _cache(internalQueryCacheSize.load()) {}

PlanCache::PlanCache(const std::string& ns) : _cache(internalQueryCacheSize.load()), _ns(ns) {}

PlanCache::~PlanCache() {}

/**
 * Traverses expression tree pre-order.
 * Appends an encoding of each node's match type and path name
 * to the output stream.
 */
void PlanCache::encodeKeyForMatch(const MatchExpression* tree, StringBuilder* keyBuilder) const {
    // Encode match type and path.
    *keyBuilder << encodeMatchType(tree->matchType());

    encodeUserString(tree->path(), keyBuilder);

    // GEO and GEO_NEAR require additional encoding.
    if (MatchExpression::GEO == tree->matchType()) {
        encodeGeoMatchExpression(static_cast<const GeoMatchExpression*>(tree), keyBuilder);
    } else if (MatchExpression::GEO_NEAR == tree->matchType()) {
        encodeGeoNearMatchExpression(static_cast<const GeoNearMatchExpression*>(tree), keyBuilder);
    }

    // We must encode the presence of any regular expressions within MATCH_IN to ensure correctness.
    if (MatchExpression::MATCH_IN == tree->matchType()) {
        if (!static_cast<const InMatchExpression*>(tree)->getRegexes().empty()) {
            encodeUserString("_re"_sd, keyBuilder);
        }
    }

    // Encode indexability.
    const IndexToDiscriminatorMap& discriminators =
        _indexabilityState.getDiscriminators(tree->path());
    if (!discriminators.empty()) {
        *keyBuilder << kEncodeDiscriminatorsBegin;
        // For each discriminator on this path, append the character '0' or '1'.
        for (auto&& indexAndDiscriminatorPair : discriminators) {
            *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
        }
        *keyBuilder << kEncodeDiscriminatorsEnd;
    }

    // Traverse child nodes.
    // Enclose children in [].
    if (tree->numChildren() > 0) {
        *keyBuilder << kEncodeChildrenBegin;
    }
    // Use comma to separate children encoding.
    for (size_t i = 0; i < tree->numChildren(); ++i) {
        if (i > 0) {
            *keyBuilder << kEncodeChildrenSeparator;
        }
        encodeKeyForMatch(tree->getChild(i), keyBuilder);
    }
    if (tree->numChildren() > 0) {
        *keyBuilder << kEncodeChildrenEnd;
    }
}

/**
 * Encodes sort order into cache key.
 * Sort order is normalized because it provided by
 * QueryRequest.
 */
void PlanCache::encodeKeyForSort(const BSONObj& sortObj, StringBuilder* keyBuilder) const {
    if (sortObj.isEmpty()) {
        return;
    }

    *keyBuilder << kEncodeSortSection;

    BSONObjIterator it(sortObj);
    while (it.more()) {
        BSONElement elt = it.next();
        // $meta text score
        if (QueryRequest::isTextScoreMeta(elt)) {
            *keyBuilder << "t";
        }
        // Ascending
        else if (elt.numberInt() == 1) {
            *keyBuilder << "a";
        }
        // Descending
        else {
            *keyBuilder << "d";
        }
        encodeUserString(elt.fieldName(), keyBuilder);

        // Sort argument separator
        if (it.more()) {
            *keyBuilder << ",";
        }
    }
}

/**
 * Encodes parsed projection into cache key.
 * Does a simple toString() on each projected field
 * in the BSON object.
 * Orders the encoded elements in the projection by field name.
 * This handles all the special projection types ($meta, $elemMatch, etc.)
 */
void PlanCache::encodeKeyForProj(const BSONObj& projObj, StringBuilder* keyBuilder) const {
    // Sorts the BSON elements by field name using a map.
    std::map<StringData, BSONElement> elements;

    BSONObjIterator it(projObj);
    while (it.more()) {
        BSONElement elt = it.next();
        StringData fieldName = elt.fieldNameStringData();

        // Internal callers may add $-prefixed fields to the projection. These are not part of a
        // user query, and therefore are not considered part of the cache key.
        if (fieldName[0] == '$') {
            continue;
        }

        elements[fieldName] = elt;
    }

    if (!elements.empty()) {
        *keyBuilder << kEncodeProjectionSection;
    }

    // Read elements in order of field name
    for (std::map<StringData, BSONElement>::const_iterator i = elements.begin();
         i != elements.end();
         ++i) {
        const BSONElement& elt = (*i).second;

        if (elt.type() != BSONType::Object) {
            // For inclusion/exclusion projections, we encode as "i" or "e".
            *keyBuilder << (elt.trueValue() ? "i" : "e");
        } else {
            // For projection operators, we use the verbatim string encoding of the element.
            encodeUserString(elt.toString(false,   // includeFieldName
                                          false),  // full
                             keyBuilder);
        }

        encodeUserString(elt.fieldName(), keyBuilder);
    }
}

Status PlanCache::add(const CanonicalQuery& query,
                      const std::vector<QuerySolution*>& solns,
                      std::unique_ptr<PlanRankingDecision> why,
                      Date_t now) {
    invariant(why);

    if (solns.empty()) {
        return Status(ErrorCodes::BadValue, "no solutions provided");
    }

    if (why->stats.size() != solns.size()) {
        return Status(ErrorCodes::BadValue, "number of stats in decision must match solutions");
    }

    if (why->scores.size() != solns.size()) {
        return Status(ErrorCodes::BadValue, "number of scores in decision must match solutions");
    }

    if (why->candidateOrder.size() != solns.size()) {
        return Status(ErrorCodes::BadValue,
                      "candidate ordering entries in decision must match solutions");
    }

    auto entry(PlanCacheEntry::create(solns, std::move(why), query, now));

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    std::unique_ptr<PlanCacheEntry> evictedEntry = _cache.add(computeKey(query), entry.release());

    if (NULL != evictedEntry.get()) {
        LOG(1) << _ns << ": plan cache maximum size exceeded - "
               << "removed least recently used entry " << redact(evictedEntry->toString());
    }

    return Status::OK();
}

Status PlanCache::get(const CanonicalQuery& query, CachedSolution** crOut) const {
    PlanCacheKey key = computeKey(query);
    verify(crOut);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        return cacheStatus;
    }
    invariant(entry);

    *crOut = new CachedSolution(key, *entry);

    return Status::OK();
}

Status PlanCache::feedback(const CanonicalQuery& cq, PlanCacheEntryFeedback* feedback) {
    if (NULL == feedback) {
        return Status(ErrorCodes::BadValue, "feedback is NULL");
    }
    std::unique_ptr<PlanCacheEntryFeedback> autoFeedback(feedback);
    PlanCacheKey ck = computeKey(cq);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry;
    Status cacheStatus = _cache.get(ck, &entry);
    if (!cacheStatus.isOK()) {
        return cacheStatus;
    }
    invariant(entry);

    // We store up to a constant number of feedback entries.
    if (entry->feedback.size() < static_cast<size_t>(internalQueryCacheFeedbacksStored.load())) {
        entry->feedback.push_back(autoFeedback.release());
    }

    return Status::OK();
}

Status PlanCache::remove(const CanonicalQuery& canonicalQuery) {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.remove(computeKey(canonicalQuery));
}

void PlanCache::clear() {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    _cache.clear();
}

PlanCacheKey PlanCache::computeKey(const CanonicalQuery& cq) const {
    StringBuilder keyBuilder;
    encodeKeyForMatch(cq.root(), &keyBuilder);
    encodeKeyForSort(cq.getQueryRequest().getSort(), &keyBuilder);
    encodeKeyForProj(cq.getQueryRequest().getProj(), &keyBuilder);
    return keyBuilder.str();
}

Status PlanCache::getEntry(const CanonicalQuery& query, PlanCacheEntry** entryOut) const {
    PlanCacheKey key = computeKey(query);
    verify(entryOut);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        return cacheStatus;
    }
    invariant(entry);

    *entryOut = entry->clone();

    return Status::OK();
}

std::vector<PlanCacheEntry*> PlanCache::getAllEntries() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    std::vector<PlanCacheEntry*> entries;
    typedef std::list<std::pair<PlanCacheKey, PlanCacheEntry*>>::const_iterator ConstIterator;
    for (ConstIterator i = _cache.begin(); i != _cache.end(); i++) {
        PlanCacheEntry* entry = i->second;
        entries.push_back(entry->clone());
    }

    return entries;
}

bool PlanCache::contains(const CanonicalQuery& cq) const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.hasKey(computeKey(cq));
}

size_t PlanCache::size() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.size();
}

void PlanCache::notifyOfIndexEntries(const std::vector<IndexEntry>& indexEntries) {
    _indexabilityState.updateDiscriminators(indexEntries);
}

}  // namespace mongo
