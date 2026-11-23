#include "Driver.h"
#include <iterator>
#include <utility>

namespace display {
namespace {
void MonitorMode(IDDCX_MONITOR_MODE& out, const CtlDisplayMode& m, const Profile& p)
{
    out = p.DescribeMode(m);
}

template<class T> void TargetMode(T& out, const CtlDisplayMode& m, const Profile& p)
{
    out = {};
    out.Size = sizeof(out);
    out.TargetVideoSignalInfo.targetVideoSignalInfo = Timing(m, false, p);
}

#if IDDCX_VERSION_MINOR >= 10
IDDCX_BITS_PER_COMPONENT ColorDepth(const Profile& p)
{
    return p.Hdr() ? IDDCX_BITS_PER_COMPONENT_8 | IDDCX_BITS_PER_COMPONENT_10 : IDDCX_BITS_PER_COMPONENT_8;
}

void MonitorMode(IDDCX_MONITOR_MODE2& out, const CtlDisplayMode& m, const Profile& p)
{
    const auto base = p.DescribeMode(m);
    out = {};
    out.Size = sizeof(out);
    out.Origin = base.Origin;
    out.MonitorVideoSignalInfo = base.MonitorVideoSignalInfo;
    out.BitsPerComponent.Rgb = ColorDepth(p);
}

template<> void TargetMode(IDDCX_TARGET_MODE2& out, const CtlDisplayMode& m, const Profile& p)
{
    out = {};
    out.Size = sizeof(out);
    out.TargetVideoSignalInfo.targetVideoSignalInfo = Timing(m, false, p);
    out.BitsPerComponent.Rgb = ColorDepth(p);
}
#endif

template<class Input> NTSTATUS Parse(const Input* in, IDARG_OUT_PARSEMONITORDESCRIPTION* out)
{
    const auto p = Descriptors().Find(in->MonitorDescription);
    if (!p) return STATUS_INVALID_PARAMETER;
    out->MonitorModeBufferOutputCount = static_cast<UINT>(p->modes.size());
    out->PreferredMonitorModeIdx = 0;
    if (!in->pMonitorModes || !in->MonitorModeBufferInputCount) return STATUS_SUCCESS;
    if (in->MonitorModeBufferInputCount < p->modes.size()) return STATUS_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < p->modes.size(); ++i) MonitorMode(in->pMonitorModes[i], p->modes[i], *p);
    return STATUS_SUCCESS;
}

EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES DefaultModes;
NTSTATUS DefaultModes(IDDCX_MONITOR handle, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in,
                      IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out)
{
    const auto& p = *GetMonitor(handle).profile;
    out->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(p.modes.size());
    out->PreferredMonitorModeIdx = 0;
    if (!in->pDefaultMonitorModes || !in->DefaultMonitorModeBufferInputCount) return STATUS_SUCCESS;
    if (in->DefaultMonitorModeBufferInputCount < p.modes.size()) return STATUS_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < p.modes.size(); ++i) MonitorMode(in->pDefaultMonitorModes[i], p.modes[i], p);
    return STATUS_SUCCESS;
}

template<class Input> NTSTATUS Query(IDDCX_MONITOR handle, const Input* in, IDARG_OUT_QUERYTARGETMODES* out)
{
    const auto& monitor = GetMonitor(handle);
    const auto snapshot = monitor.QueryTargets();
    const auto& modes = snapshot.SourceModes();
    const auto fits = [limit = snapshot.limit](const auto& mode) { return FitsSurface(mode, limit); };
    const UINT count = static_cast<UINT>(std::count_if(modes.begin(), modes.end(), fits));
    out->TargetModeBufferOutputCount = count;
    if (!in->pTargetModes || !in->TargetModeBufferInputCount) return STATUS_SUCCESS;
    if (in->TargetModeBufferInputCount < count) return STATUS_BUFFER_TOO_SMALL;
    UINT index = 0;
    for (const auto& mode : modes)
        if (fits(mode)) TargetMode(in->pTargetModes[index++], mode, *monitor.profile);
    return STATUS_SUCCESS;
}

EVT_IDD_CX_ADAPTER_COMMIT_MODES Commit;
NTSTATUS Commit(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES* in)
{
    for (UINT i = 0; i < in->PathCount; ++i) {
        const auto& path = in->pPaths[i];
        const bool active = (path.Flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
        GetMonitor(path.MonitorObject).color.Commit(active ? IDD_WIRE_SDR : 0, active ? IDD_WIRE_8BPC : 0);
    }
    return STATUS_SUCCESS;
}

template<class Target, class Input, class Update>
NTSTATUS SendTargets(Monitor& monitor, const Modes& modes, Update update)
{
    std::vector<Target> list(modes.size());
    for (size_t i = 0; i < modes.size(); ++i) TargetMode(list[i], modes[i], *monitor.profile);
    Input input = {IDDCX_UPDATE_REASON_OTHER, static_cast<UINT>(list.size()), list.data()};
    return update(monitor.handle, &input);
}

std::shared_ptr<const Modes> Available(const std::shared_ptr<const Modes>& modes, UINT limit)
{
    const auto fits = [limit](const auto& mode) { return FitsSurface(mode, limit); };
    if (std::all_of(modes->begin(), modes->end(), fits)) return modes;
    auto result = std::make_shared<Modes>();
    std::copy_if(modes->begin(), modes->end(), std::back_inserter(*result), fits);
    return result;
}
} // namespace

void ConfigureModes(IDD_CX_CLIENT_CONFIG& config)
{
    const auto parse = [](const auto* in, IDARG_OUT_PARSEMONITORDESCRIPTION* out) { return Parse(in, out); };
    const auto query = [](IDDCX_MONITOR h, const auto* in, IDARG_OUT_QUERYTARGETMODES* out) { return Query(h, in, out); };
    config.EvtIddCxMonitorGetDefaultDescriptionModes = DefaultModes;
#if IDDCX_VERSION_MINOR >= 10
    if (AdvancedColorAvailable()) {
        config.EvtIddCxParseMonitorDescription2 = parse;
        config.EvtIddCxMonitorQueryTargetModes2 = query;
        return;
    }
#endif
    config.EvtIddCxParseMonitorDescription = parse;
    config.EvtIddCxMonitorQueryTargetModes = query;
    config.EvtIddCxAdapterCommitModes = Commit;
}

Monitor::Monitor(ProfilePtr p, InitialTargets initial, WDFWORKITEM work) : profile(std::move(p)), modeWork(work),
    renderLimit(initial.limit), lastAdapter(initial.adapter), adapters(std::move(initial.adapters)),
    publishedLimit(initial.limit), requested(std::move(initial.requested)), targets(Available(requested, initial.limit))
{
    RememberAdapter(lastAdapter);
}

void Monitor::RememberAdapter(LUID adapter)
{
    if (!SameAdapter(adapter, {}) && std::none_of(adapters.begin(), adapters.end(),
        [adapter](LUID known) { return SameAdapter(known, adapter); })) adapters.push_back(adapter);
}

NTSTATUS Monitor::PublishTargets(const Modes& modes)
{
    try {
        return SetTargets(SameModes(*requested, modes) ? requested : std::make_shared<const Modes>(modes));
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS Monitor::RefreshTargets(RenderChange change)
{
    return SetTargets(requested, &change);
}

void Monitor::BeginRenderAssignment()
{
    std::lock_guard<std::mutex> guard(renderLock);
    pendingPreference = 0;
}

bool Monitor::ObserveRenderLevel(D3D_FEATURE_LEVEL level, LUID adapter)
{
    std::lock_guard<std::mutex> guard(renderLock);
    if (withdrawing) return false;
    RememberAdapter(adapter);
    assigned = true;
    lastAdapterLimit = TextureDimension(level);
    lastAdapter = adapter;
    // A preference submitted while SetDevice was binding still applies to this assignment.
    const bool changed = ChangeLimit(pendingPreference ? (std::min)(lastAdapterLimit, pendingPreference) : lastAdapterLimit);
    if (changed) WdfWorkItemEnqueue(modeWork);
    return changed;
}

bool Monitor::PrepareRenderLimit(UINT limit)
{
    std::lock_guard<std::mutex> guard(renderLock);
    pendingPreference = limit;
    return ChangeLimit(assigned ? (std::min)(lastAdapterLimit, limit) : limit);
}

bool Monitor::ChangeLimit(UINT limit)
{
    ++renderRevision;
    const bool changed = renderLimit != limit;
    renderLimit = limit;
    return republishTargets || (limit && (changed || publishedLimit != limit));
}

bool Monitor::PendingRenderChange(RenderChange& change) const
{
    std::lock_guard<std::mutex> guard(renderLock);
    change = {renderLimit ? renderLimit : publishedLimit, renderRevision};
    return republishTargets || change.limit != publishedLimit;
}

void Monitor::ReleaseRenderAssignment()
{
    std::lock_guard<std::mutex> guard(renderLock);
    if (!assigned) return;
    assigned = false;
    // Without a pending preference, retain the capability published before unassignment.
    if (ChangeLimit(pendingPreference) && !withdrawing) WdfWorkItemEnqueue(modeWork);
}

bool Monitor::BeginWithdrawal(RenderChange change)
{
    std::lock_guard<std::mutex> path(pathLock);
    std::lock_guard<std::mutex> render(renderLock);
    if (withdrawing || renderRevision != change.revision || !assigned || lastAdapterLimit != change.limit) return false;
    // IddCx can unassign synchronously during departure. Reserve it here, then release both locks.
    withdrawing = true;
    return true;
}

void Monitor::CancelWithdrawal()
{
    std::lock_guard<std::mutex> path(pathLock);
    std::lock_guard<std::mutex> render(renderLock);
    withdrawing = false;
}

UINT Monitor::AssignedLimit() const
{
    std::lock_guard<std::mutex> guard(renderLock);
    return assigned ? renderLimit : 0;
}

ReplacementTargets Monitor::PrepareReplacement(std::shared_ptr<const Modes> modes, LUID preferred) const
{
    std::lock_guard<std::mutex> guard(renderLock);
    const bool hasAdapter = assigned && (lastAdapter.LowPart || lastAdapter.HighPart);
    return {{publishedLimit, requested, lastAdapter, adapters},
            {assigned ? renderLimit : 0, std::move(modes), hasAdapter ? lastAdapter : preferred, adapters}};
}

NTSTATUS Monitor::SetTargets(std::shared_ptr<const Modes> next, const RenderChange* pending)
{
    RenderChange change;
    bool republish;
    std::shared_ptr<const Modes> previous;
    {
        std::lock_guard<std::mutex> guard(renderLock);
        if (pending && pending->revision != renderRevision) return STATUS_SUCCESS;
        change = {renderLimit ? renderLimit : publishedLimit, renderRevision};
        republish = republishTargets;
        previous = targets;
    }
    try {
        const auto available = Available(next, change.limit);
        if (available->empty()) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
        if (republish || !SameModes(*previous, *available)) {
            SetQueryTargets(available); // IddCx may query synchronously during the update.
            NTSTATUS status;
#if IDDCX_VERSION_MINOR >= 10
            if (AdvancedColorAvailable())
                status = SendTargets<IDDCX_TARGET_MODE2, IDARG_IN_UPDATEMODES2>(*this, *available, IddCxMonitorUpdateModes2);
            else
#endif
                status = SendTargets<IDDCX_TARGET_MODE, IDARG_IN_UPDATEMODES>(*this, *available, IddCxMonitorUpdateModes);
            if (!NT_SUCCESS(status)) {
                SetQueryTargets(previous);
                return status;
            }
        }
        return CommitTargets(std::move(next), change);
    } catch (const std::bad_alloc&) {
        SetQueryTargets(previous);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS Monitor::CommitTargets(std::shared_ptr<const Modes> next, RenderChange change)
{
    std::lock_guard<std::mutex> guard(renderLock);
    const UINT currentLimit = renderLimit ? renderLimit : publishedLimit;
    if (currentLimit != change.limit && !HasRenderTarget(*next, currentLimit)) {
        // Queries must see the last accepted request while the work item corrects the IddCx list.
        targets = requested;
        republishTargets = true;
        WdfWorkItemEnqueue(modeWork);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    republishTargets = false;
    const UINT previousLimit = std::exchange(publishedLimit, change.limit);
    if (renderRevision != change.revision && !renderLimit && ChangeLimit(previousLimit))
        WdfWorkItemEnqueue(modeWork);
    requested = std::move(next);
    return STATUS_SUCCESS;
}
} // namespace display
