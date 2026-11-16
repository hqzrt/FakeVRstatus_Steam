#include <openvr_driver.h>
#include <windows.h>
#include <cstring>
#include <cmath>

using namespace vr;

static const int32_t  kWinX = -8000, kWinY = -8000;
static const uint32_t kWinW = 256,   kWinH = 128;

static double NowSec()
{
    static LARGE_INTEGER f = {}, t0 = {};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart - t0.QuadPart) / double(f.QuadPart);
}

class FakeDisplay
{
public:
    virtual void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h)
    { *x = kWinX; *y = kWinY; *w = kWinW; *h = kWinH; }
    virtual bool IsDisplayOnDesktop() { return true; }
    virtual bool IsDisplayRealDisplay() { return false; }
    virtual void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) { *w = kWinW; *h = kWinH; }
    virtual void GetEyeOutputViewport(EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
    { *y = 0; *w = kWinW / 2; *h = kWinH; *x = (eye == Eye_Left) ? 0 : kWinW / 2; }
    virtual void GetProjectionRaw(EVREye, float *l, float *r, float *t, float *b)
    { *l = -1.f; *r = 1.f; *t = -1.f; *b = 1.f; }
    virtual DistortionCoordinates_t *ComputeDistortion(DistortionCoordinates_t *out, EVREye, float u, float v)
    {
        out->rfRed[0] = out->rfGreen[0] = out->rfBlue[0] = u;
        out->rfRed[1] = out->rfGreen[1] = out->rfBlue[1] = v;
        return out;
    }
    virtual bool ComputeInverseDistortion(HmdVector2_t *, EVREye, uint32_t, float, float) { return false; }
};

class FakeHmd
{
public:
    virtual EVRInitError Activate(uint32_t id)
    {
        m_id = id;
        PropertyContainerHandle_t c = VRProperties()->TrackedDeviceToPropertyContainer(id);
        VRProperties()->SetStringProperty(c, Prop_ModelNumber_String, "Fake HMD");
        VRProperties()->SetStringProperty(c, Prop_ManufacturerName_String, "FakeHMD");
        VRProperties()->SetStringProperty(c, Prop_TrackingSystemName_String, "fakehmd");
        VRProperties()->SetFloatProperty(c, Prop_UserIpdMeters_Float, 0.063f);
        VRProperties()->SetFloatProperty(c, Prop_UserHeadToEyeDepthMeters_Float, 0.f);
        VRProperties()->SetFloatProperty(c, Prop_DisplayFrequency_Float, 30.f);
        VRProperties()->SetFloatProperty(c, Prop_SecondsFromVsyncToPhotons_Float, 0.1f);
        VRProperties()->SetUint64Property(c, Prop_CurrentUniverseId_Uint64, 2);
        VRProperties()->SetBoolProperty(c, Prop_IsOnDesktop_Bool, true);
        VRProperties()->SetBoolProperty(c, Prop_DisplayDebugMode_Bool, true);
        VRProperties()->SetBoolProperty(c, Prop_ContainsProximitySensor_Bool, true);
        VRDriverInput()->CreateBooleanComponent(c, "/proximity", &m_prox);
        return VRInitError_None;
    }
    virtual void Deactivate() { m_id = k_unTrackedDeviceIndexInvalid; }
    virtual void EnterStandby() {}
    virtual void *GetComponent(const char *name)
    {
        if (strcmp(name, IVRDisplayComponent_Version) == 0) return &m_display;
        return nullptr;
    }
    virtual void DebugRequest(const char *, char *buf, uint32_t size) { if (size) buf[0] = 0; }
    virtual DriverPose_t *GetPose(DriverPose_t *out)
    {
        Fill(*out);
        return out;
    }

    void Fill(DriverPose_t &p)
    {
        memset(&p, 0, sizeof(p));
        p.qWorldFromDriverRotation.w = 1; p.qDriverFromHeadRotation.w = 1;
        double t = NowSec();
        double yaw = 0.05 * sin(t * 2.0);
        p.qRotation.w = cos(yaw / 2); p.qRotation.y = sin(yaw / 2);
        p.vecPosition[0] = 0.02 * sin(t * 1.1);
        p.vecPosition[1] = 1.7 + 0.02 * sin(t * 1.7);
        p.vecVelocity[0] = 0.022 * cos(t * 1.1);
        p.vecVelocity[1] = 0.034 * cos(t * 1.7);
        p.vecAngularVelocity[1] = 0.1 * cos(t * 2.0);
        p.poseIsValid = true; p.result = TrackingResult_Running_OK; p.deviceIsConnected = true;
    }
    void RunFrame()
    {
        if (m_id == k_unTrackedDeviceIndexInvalid) return;
        DriverPose_t p; Fill(p);
        VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, p, sizeof(p));
        if (m_prox != k_ulInvalidInputComponentHandle) VRDriverInput()->UpdateBooleanComponent(m_prox, true, 0);
    }

private:
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    VRInputComponentHandle_t m_prox = k_ulInvalidInputComponentHandle;
    FakeDisplay m_display;
};

class FakeProvider
{
public:
    virtual EVRInitError Init(IVRDriverContext *ctx)
    {
        VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
        VRServerDriverHost()->TrackedDeviceAdded("FAKEHMD-0001", TrackedDeviceClass_HMD,
                                                 reinterpret_cast<ITrackedDeviceServerDriver *>(&m_hmd));
        return VRInitError_None;
    }
    virtual void Cleanup() { VR_CLEANUP_SERVER_DRIVER_CONTEXT(); }
    virtual const char *const *GetInterfaceVersions() { return k_InterfaceVersions; }
    virtual void RunFrame()
    {
        m_hmd.RunFrame();
        VREvent_t e;
        while (VRServerDriverHost()->PollNextEvent(&e, sizeof(e))) {}
    }
    virtual bool ShouldBlockStandbyMode() { return true; }
    virtual void EnterStandby() {}
    virtual void LeaveStandby() {}

private:
    FakeHmd m_hmd;
};

static FakeProvider g_provider;

extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *name, int *ret)
{
    if (strcmp(IServerTrackedDeviceProvider_Version, name) == 0) return &g_provider;
    if (ret) *ret = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
