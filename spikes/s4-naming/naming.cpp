// Prototype of naming::propagate and IResolver (docs/interfaces/naming.h, PLAN.md 3.3).
// This file knows nothing about the ground-truth oracle (truth.cpp) and nothing about the script.
#include "s4.h"
#include <cstdlib>
#include <cstring>

namespace s4 {

void PropStats::add(const PropStats& o) {
    for (int k = 0; k < 3; ++k) {
        sweep[k] += o.sweep[k]; inherited[k] += o.inherited[k]; split[k] += o.split[k]; merged[k] += o.merged[k];
        generated[k] += o.generated[k]; neighbour[k] += o.neighbour[k]; inferred[k] += o.inferred[k];
        ambiguous[k] += o.ambiguous[k]; unnamed[k] += o.unnamed[k];
    }
}

NameId NameTable::intern(TopoName& n, bool groupInHash) {
    std::string buf;
    buf.push_back((char)n.kind);
    buf += n.origin; buf.push_back('\0');
    buf += n.role; buf.push_back('\0');
    for (NameId p : n.parents) buf.append((const char*)&p, sizeof p);
    n.family = hashStr(buf);
    for (NameId p : n.disc) { buf.push_back('d'); buf.append((const char*)&p, sizeof p); }
    buf.append((const char*)&n.ordinal, sizeof n.ordinal);
    if (groupInHash) buf.append((const char*)&n.group, sizeof n.group);
    n.id = hashStr(buf);
    auto it = t.find(n.id);
    if (it == t.end()) { t.emplace(n.id, n); families[n.family].push_back(n.id); }
    else if (it->second.origin != n.origin || it->second.role != n.role || it->second.parents != n.parents ||
             it->second.ordinal != n.ordinal || it->second.kind != n.kind) {
        std::fprintf(stderr, "FATAL: 64-bit name hash collision\n");
        std::abort();
    }
    return n.id;
}
std::size_t NameTable::bytesHash256() const {
    std::size_t b = 0;
    for (auto& [id, n] : t) b += 32 + 1 + 16 + n.role.size() + 1 + 2 + 32 * (n.parents.size() + n.disc.size()) + 4 + 4;
    return b;
}

namespace {
struct Pend { SubId r; TopoName n; int srcInput{-1}; SubId src{}; };
using SrcKey = std::pair<int, SubId>;
}

ElementMap propagate(const NamingInput& in, NameTable& nt, Lineage& lin, const Rules& R, PropStats& st) {
    ElementMap out;
    out.idx = in.result;
    const ShapeIdx& res = *in.result;
    for (int k = 0; k < 3; ++k) out.names[k].assign(res.count((Kind)k), {});

    std::map<SrcKey, std::vector<NameId>> splitChildren;
    // Assign ordinals inside each sibling group by fingerprint (PLAN 3.3 rule 4) and intern.
    auto named = [&](Kind k, int i) { return !out.names[k][i - 1].empty(); };
    auto primary = [&](Kind k, int i) { return out.names[k][i - 1][0]; };
    // A7: names of the faces "around" a pending element, excluding the faces that already define it.
    auto ring = [&](SubId r, bool currentFeatureOnly) {
        std::vector<int> fs;
        if (r.kind == KFace) fs = res.around(KFace, r.index);
        else if (r.kind == KEdge) {
            std::vector<int> own = res.facesOfEdge(r.index);
            for (int v : res.verticesOfEdge(r.index)) for (int f : res.facesOfVertex(v)) if (std::find(own.begin(), own.end(), f) == own.end()) fs.push_back(f);
        } else {
            std::vector<int> own = res.facesOfVertex(r.index);
            for (int e : res.edgesOfVertex(r.index)) for (int v : res.verticesOfEdge(e)) if (v != r.index)
                for (int f : res.facesOfVertex(v)) if (std::find(own.begin(), own.end(), f) == own.end()) fs.push_back(f);
        }
        std::vector<NameId> out2;
        for (int f : fs) {
            if (!named(KFace, f)) continue;
            NameId n = primary(KFace, f);
            if (currentFeatureOnly) { auto it = nt.t.find(n); if (it == nt.t.end() || it->second.origin != in.feature) continue; }
            out2.push_back(n);
        }
        std::sort(out2.begin(), out2.end()); out2.erase(std::unique(out2.begin(), out2.end()), out2.end());
        return out2;
    };
    // Assign discriminators / ordinals inside each sibling group and intern.
    // v0: ordinal by fingerprint (PLAN 3.3 rule 4). A7: first try to tell siblings apart topologically.
    auto flush = [&](std::vector<Pend>& pend) {
        std::map<std::pair<std::string, std::vector<NameId>>, std::vector<int>> groups;
        for (int i = 0; i < (int)pend.size(); ++i) groups[{pend[i].n.role, pend[i].n.parents}].push_back(i);
        for (auto& [key, g] : groups) {
            if (g.size() > 1 && R.topoDisc) {
                for (int pass = 0; pass < 2; ++pass) {
                    std::set<std::vector<NameId>> seen; bool distinct = true;
                    for (int i : g) { pend[i].n.disc = ring(pend[i].r, pass == 0); if (pend[i].n.disc.empty() || !seen.insert(pend[i].n.disc).second) distinct = false; }
                    if (distinct) break;
                }
            }
            std::map<std::vector<NameId>, std::vector<int>> sub;
            for (int i : g) sub[pend[i].n.disc].push_back(i);
            for (auto& [disc, sg] : sub) {
                if (sg.size() > 1)
                    std::stable_sort(sg.begin(), sg.end(), [&](int a, int b) {
                        return res.fingerprint(pend[a].r).key() < res.fingerprint(pend[b].r).key();
                    });
                for (int o = 0; o < (int)sg.size(); ++o) {
                    Pend& p = pend[sg[o]];
                    p.n.ordinal = (std::uint32_t)o;
                    p.n.group = (std::uint32_t)sg.size();
                    NameId id = nt.intern(p.n, R.groupInHash);
                    out.names[p.r.kind][p.r.index - 1].push_back(id);
                    if (sg.size() > 1) st.ambiguous[p.r.kind]++;
                    if (p.srcInput >= 0) splitChildren[{p.srcInput, p.src}].push_back(id);
                }
            }
        }
        for (auto& p : pend) { auto& v = out.names[p.r.kind][p.r.index - 1]; std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end()); }
        pend.clear();
    };

    // ---- 1. sweep faces (rule 1): the kernel says which sketch entity made each face
    if (in.origins) {
        std::vector<Pend> pend;
        for (const SweepOrigin& o : *in.origins) {
            Pend p; p.r = o.face; p.n.kind = KFace; p.n.origin = in.feature; p.n.role = in.role + o.role; p.n.parents = {o.parent};
            pend.push_back(p); st.sweep[KFace]++;
        }
        flush(pend);
    }

    // ---- 2. history (rule 2)
    std::map<SrcKey, std::vector<SubId>> modOut;
    std::vector<std::vector<SrcKey>> modSrc[3], genSrc[3];
    for (int k = 0; k < 3; ++k) { modSrc[k].resize(res.count((Kind)k)); genSrc[k].resize(res.count((Kind)k)); }
    if (in.hist) {
        for (const HistoryEntry& e : in.hist->entries) {
            SrcKey sk{e.input, e.src};
            if (e.rel == HistoryEntry::Deleted) continue;
            for (SubId r : e.res) {
                if (e.rel == HistoryEntry::Modified && r.kind == e.src.kind) { modSrc[r.kind][r.index - 1].push_back(sk); modOut[sk].push_back(r); }
                else genSrc[r.kind][r.index - 1].push_back(sk);
            }
        }
    }
    auto inNames = [&](const SrcKey& s) -> const std::vector<NameId>& { return in.inputs[s.first]->of(s.second); };

    for (int k = 0; k < 3; ++k) {
        std::vector<Pend> pend;
        for (int r = 1; r <= res.count((Kind)k); ++r) {
            if (named((Kind)k, r)) continue;
            auto& ms = modSrc[k][r - 1];
            auto& gs = genSrc[k][r - 1];
            if (!ms.empty()) {
                std::set<NameId> inh; int nsrc = 0; bool anySplit = false;
                for (const SrcKey& s : ms) {
                    const auto& nm = inNames(s);
                    if (nm.empty()) continue;
                    if (modOut[s].size() == 1) { inh.insert(nm.begin(), nm.end()); nsrc++; }
                    else {
                        Pend p; p.r = {(Kind)k, r}; p.n.kind = (Kind)k; p.n.origin = in.feature; p.n.role = "split"; p.n.parents = {nm[0]};
                        p.srcInput = s.first; p.src = s.second; pend.push_back(p); anySplit = true;
                    }
                }
                out.names[k][r - 1].assign(inh.begin(), inh.end());
                if (nsrc > 1) {
                    st.merged[k]++;
                    for (const SrcKey& s : ms) { const auto& nm = inNames(s); if (!nm.empty() && modOut[s].size() == 1) lin.add({LineageEvent::Merged, nm[0], {inh.begin(), inh.end()}, in.feature}); }
                } else if (nsrc == 1) st.inherited[k]++;
                if (anySplit) st.split[k]++;
            } else if (!gs.empty()) {
                std::vector<NameId> parents; std::string kinds;
                for (const SrcKey& s : gs) {
                    const auto& nm = inNames(s);
                    if (!nm.empty()) parents.push_back(nm[0]);
                    char c = "FEV"[s.second.kind]; if (kinds.find(c) == std::string::npos) kinds.push_back(c);
                }
                if (parents.empty()) continue;
                std::sort(parents.begin(), parents.end()); parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
                std::sort(kinds.begin(), kinds.end());
                Pend p; p.r = {(Kind)k, r}; p.n.kind = (Kind)k; p.n.origin = in.feature; p.n.role = in.role + ".g" + kinds + "FEV"[k]; p.n.parents = parents;
                pend.push_back(p); st.generated[k]++;
            }
        }
        flush(pend);

        // ---- A3 (amendment, off in v0): infer Modified for history-less edges / vertices from face history.
        // An input element that vanished and a result element without a name are the same logical element if
        // the images of the faces around the first are exactly the faces around the second, uniquely both ways.
        if (R.inferEdgeHistory && k != KFace && in.hist && !in.inputs.empty()) {
            auto facesAround = [&](const ShapeIdx& ix, Kind kk, int i) { return kk == KEdge ? ix.facesOfEdge(i) : ix.facesOfVertex(i); };
            std::map<std::vector<int>, std::vector<int>> resByFaces;
            for (int r = 1; r <= res.count((Kind)k); ++r)
                if (!named((Kind)k, r)) { auto fs = facesAround(res, (Kind)k, r); if (fs.size() >= 2) resByFaces[fs].push_back(r); }
            std::map<std::vector<int>, std::vector<SrcKey>> srcByFaces;
            for (int i = 0; i < (int)in.inputs.size(); ++i) {
                const ShapeIdx& ix = *in.inputs[i]->idx;
                for (int s = 1; s <= ix.count((Kind)k); ++s) {
                    SrcKey sk{i, {(Kind)k, s}};
                    if (modOut.count(sk) || inNames(sk).empty()) continue;
                    std::vector<int> img; bool ok = true;
                    for (int f : facesAround(ix, (Kind)k, s)) {
                        auto it = modOut.find({i, {KFace, f}});
                        if (it == modOut.end() || it->second.size() != 1) { ok = false; break; }
                        img.push_back(it->second[0].index);
                    }
                    if (!ok || img.size() < 2) continue;
                    std::sort(img.begin(), img.end());
                    if (std::adjacent_find(img.begin(), img.end()) != img.end()) continue;
                    srcByFaces[img].push_back(sk);
                }
            }
            for (auto& [fs, rs] : resByFaces) {
                auto it = srcByFaces.find(fs);
                if (rs.size() != 1 || it == srcByFaces.end() || it->second.size() != 1) continue;
                out.names[k][rs[0] - 1] = inNames(it->second[0]);
                modOut[it->second[0]].push_back({(Kind)k, rs[0]});
                st.inferred[k]++;
            }
        }
    }

    // ---- 3. elements with no history (rule 3): named from their neighbours
    {
        std::vector<Pend> pend;
        for (int r = 1; r <= res.count(KFace); ++r) {
            if (named(KFace, r)) continue;
            std::vector<NameId> parents;
            for (int e : res.edgesOfFace(r)) if (named(KEdge, e)) parents.push_back(primary(KEdge, e));
            std::string role = "nb.face";
            if (parents.empty()) {
                role = "nb.facef";
                for (int e : res.edgesOfFace(r)) for (int f : res.facesOfEdge(e)) if (f != r && named(KFace, f)) parents.push_back(primary(KFace, f));
            }
            if (parents.empty()) { st.unnamed[KFace]++; continue; }
            std::sort(parents.begin(), parents.end()); parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
            Pend p; p.r = {KFace, r}; p.n.kind = KFace; p.n.origin = in.feature; p.n.role = role; p.n.parents = parents;
            pend.push_back(p); st.neighbour[KFace]++;
        }
        flush(pend);
        for (int k = KEdge; k <= KVertex; ++k) {
            for (int r = 1; r <= res.count((Kind)k); ++r) {
                if (named((Kind)k, r)) continue;
                std::vector<NameId> parents;
                for (int f : (k == KEdge ? res.facesOfEdge(r) : res.facesOfVertex(r))) if (named(KFace, f)) parents.push_back(primary(KFace, f));
                if (parents.empty()) { st.unnamed[k]++; continue; }
                std::sort(parents.begin(), parents.end());
                Pend p; p.r = {(Kind)k, r}; p.n.kind = (Kind)k; p.n.origin = in.feature; p.n.role = k == KEdge ? "nb.edge" : "nb.vertex"; p.n.parents = parents;
                pend.push_back(p); st.neighbour[k]++;
            }
            flush(pend);
        }
    }

    // ---- lineage: what happened to every named input element
    for (int i = 0; i < (int)in.inputs.size(); ++i) {
        const ElementMap& im = *in.inputs[i];
        for (int k = 0; k < 3; ++k)
            for (int s = 1; s <= im.idx->count((Kind)k); ++s) {
                SrcKey sk{i, {(Kind)k, s}};
                const auto& nm = im.of(sk.second);
                if (nm.empty()) continue;
                auto it = modOut.find(sk);
                if (it == modOut.end() || it->second.empty()) { for (NameId n : nm) lin.add({LineageEvent::Deleted, n, {}, in.feature}); }
                else if (it->second.size() > 1) { for (NameId n : nm) lin.add({LineageEvent::Split, n, splitChildren[sk], in.feature}); }
            }
    }

    for (int k = 0; k < 3; ++k)
        for (int r = 1; r <= res.count((Kind)k); ++r)
            for (NameId n : out.names[k][r - 1]) {
                auto [it, fresh] = out.byName.emplace(n, SubId{(Kind)k, r});
                if (!fresh && it->second != SubId{(Kind)k, r}) {
                    std::fprintf(stderr, "FATAL: one name on two elements (feature %s)\n", in.feature.c_str());
                    std::abort();
                }
            }
    return out;
}

// ------------------------------------------------------------------ references and resolution
TopoRef makeRef(const ElementMap& m, const NameTable& nt, SubId s) {
    TopoRef r; r.kind = s.kind; r.names = m.of(s); r.hint = m.idx->fingerprint(s);
    for (NameId n : r.names) { auto it = nt.t.find(n); if (it != nt.t.end()) r.recs.push_back(it->second); }
    for (int f : m.idx->around(s.kind, s.index)) { const auto& nm = m.names[KFace][f - 1]; if (!nm.empty()) r.nbNames.push_back(nm[0]); }
    return r;
}

bool fpClose(const Fingerprint& a, const Fingerprint& b) {
    auto near = [](std::int64_t x, std::int64_t y, std::int64_t t) { return std::llabs(x - y) <= t; };
    if (a.type != b.type || a.neighbourCount != b.neighbourCount) return false;
    std::int64_t at = 10 + (std::int64_t)(1e-7 * (double)std::llabs(a.qArea));
    return near(a.qArea, b.qArea, at) && near(a.qCx, b.qCx, 10) && near(a.qCy, b.qCy, 10) && near(a.qCz, b.qCz, 10) &&
           near(a.qRadius, b.qRadius, 10) && near(a.qAxisX, b.qAxisX, 1000) && near(a.qAxisY, b.qAxisY, 1000) && near(a.qAxisZ, b.qAxisZ, 1000);
}

static void lookup(NameId n, const ElementMap& m, const Lineage& lin, std::set<SubId>& out, bool& split, bool& deleted, int depth) {
    auto it = m.byName.find(n);
    if (it != m.byName.end()) { out.insert(it->second); return; }
    if (depth > 64) return;
    auto rg = lin.byFrom.equal_range(n);
    for (auto e = rg.first; e != rg.second; ++e) {
        const LineageEvent& ev = lin.ev[e->second];
        if (ev.kind == LineageEvent::Deleted) deleted = true;
        else {
            if (ev.kind == LineageEvent::Split) split = true;
            for (NameId c : ev.to) lookup(c, m, lin, out, split, deleted, depth + 1);
        }
    }
}

Resolved resolve(const TopoRef& ref, const ElementMap& m, const NameTable& nt, const Lineage& lin, const Rules& R) {
    Resolved res;
    std::set<SubId> U; bool split = false, deleted = false; int found = 0;
    for (NameId n : ref.names) { size_t b = U.size(); std::set<SubId> one; lookup(n, m, lin, one, split, deleted, 0); if (!one.empty()) found++; U.insert(one.begin(), one.end()); (void)b; }
    if (!U.empty()) {
        res.elems.assign(U.begin(), U.end());
        if (U.size() == 1 && !split) {
            const auto& have = m.of(*U.begin());
            bool extra = false;
            for (NameId n : have) if (std::find(ref.names.begin(), ref.names.end(), n) == ref.names.end()) extra = true;
            if (extra) { res.state = RState::Merged; res.how = "alias"; }
            else if (found < (int)ref.names.size()) { res.state = RState::Split; res.how = "partial"; }
            else { res.state = RState::Exact; res.how = "name"; }
            if (R.hintOnAmbiguous && res.state == RState::Exact) {
                bool amb = false;
                for (NameId n : ref.names) { auto it = nt.t.find(n); if (it != nt.t.end() && it->second.group > 1) amb = true; }
                if (amb && !fpClose(ref.hint, m.idx->fingerprint(*U.begin()))) { res.elems.clear(); res.state = RState::Broken; res.how = "ambiguous-name-hint-mismatch"; U.clear(); }
            }
        } else { res.state = RState::Split; res.how = "lineage"; }
        if (!U.empty()) return res;
        return res; // A2 turned an untrusted ordinal name into Broken; never fall through to repair with a moved hint
    }
    // A5: the name is gone but its family (same origin, role, parents) exists in this build.
    //  - picked when it was the only member, now several: the logical element is in pieces -> Split (all members)
    //  - picked as one of several, now exactly one member: the pieces re-joined -> Merged
    if (R.familyFallback) {
        for (const TopoName& rec : ref.recs) {
            auto it = nt.families.find(rec.family);
            if (it == nt.families.end()) continue;
            const auto& members = it->second;
            bool single = rec.group == 1 && rec.disc.empty();
            std::set<SubId> P; bool sp = false, del = false;
            for (NameId n : members) lookup(n, m, lin, P, sp, del, 0);
            if (P.empty()) continue;
            if (single && members.size() > 1) { res.state = RState::Split; res.elems.assign(P.begin(), P.end()); res.how = "family-split"; return res; }
            if (!single && members.size() == 1 && P.size() == 1 && !sp) { res.state = RState::Merged; res.elems.assign(P.begin(), P.end()); res.how = "family-unsplit"; return res; }
        }
    }
    // A4: a piece name whose parent is whole again
    if (R.unsplitFallback) {
        std::set<SubId> P; bool sp = false, del = false;
        for (const TopoName& rec : ref.recs) {
            if (rec.role != "split" || rec.parents.size() != 1) continue;
            lookup(rec.parents[0], m, lin, P, sp, del, 0);
        }
        if (P.size() == 1 && !sp) { res.state = RState::Merged; res.elems.assign(P.begin(), P.end()); res.how = "unsplit"; return res; }
    }
    // Repaired: name gone, exactly one element matches the stored fingerprint and neighbour count.
    // A8: and the faces around the candidate are exactly what the neighbour names stored at pick time resolve to.
    if (R.repair) {
        std::vector<SubId> cand;
        for (int i = 1; i <= m.idx->count(ref.kind); ++i)
            if (fpClose(ref.hint, m.idx->fingerprint({ref.kind, i}))) cand.push_back({ref.kind, i});
        if (cand.size() == 1) {
            bool ok = true;
            if (R.repairNeedsNeighbours) {
                std::vector<int> C = m.idx->around(ref.kind, cand[0].index);
                std::set<int> all;
                for (NameId n : ref.nbNames) {
                    std::set<SubId> S; bool sp = false, del = false;
                    lookup(n, m, lin, S, sp, del, 0);
                    bool hit = false;
                    for (SubId x : S) { all.insert(x.index); if (std::find(C.begin(), C.end(), x.index) != C.end()) hit = true; }
                    if (!hit) ok = false;
                }
                for (int c : C) if (!all.count(c)) ok = false;
                if (ref.nbNames.empty()) ok = false;
            }
            if (ok) { res.state = RState::Repaired; res.elems = cand; res.how = "fingerprint"; return res; }
        }
    }
    res.state = RState::Broken;
    res.how = deleted ? "deleted" : "unknown-name";
    return res;
}

} // namespace s4
