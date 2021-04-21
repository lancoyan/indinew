/*
    INDI Explore Scientific PMC8 driver

    Copyright (C) 2017 Michael Fulbright

    Additional contributors: 
        Thomas Olson, Copyright (C) 2019
        Karl Rees, Copyright (C) 2019-2021

    Based on IEQPro driver.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
/* Experimental Mount selector switch G11 vs EXOS2 by Thomas Olson
 *
 */

#include "pmc8.h"

#include <indicom.h>
#include <connectionplugins/connectionserial.h>
#include <connectionplugins/connectiontcp.h>

#include <libnova/sidereal_time.h>

#include <memory>

#include <math.h>
#include <string.h>

/* Simulation Parameters */
#define SLEWRATE 3          /* slew rate, degrees/s */

#define MOUNTINFO_TAB "Mount Info"

#define PMC8_DEFAULT_PORT 54372
#define PMC8_DEFAULT_IP_ADDRESS "192.168.47.1"
#define PMC8_TRACKING_AUTODETECT_INTERVAL 10

static std::unique_ptr<PMC8> scope(new PMC8());

void ISGetProperties(const char *dev)
{
    scope->ISGetProperties(dev);
}

void ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int num)
{
    scope->ISNewSwitch(dev, name, states, names, num);
}

void ISNewText(const char *dev, const char *name, char *texts[], char *names[], int num)
{
    scope->ISNewText(dev, name, texts, names, num);
}

void ISNewNumber(const char *dev, const char *name, double values[], char *names[], int num)
{
    scope->ISNewNumber(dev, name, values, names, num);
}

void ISNewBLOB(const char *dev, const char *name, int sizes[], int blobsizes[], char *blobs[], char *formats[],
               char *names[], int n)
{
    INDI_UNUSED(dev);
    INDI_UNUSED(name);
    INDI_UNUSED(sizes);
    INDI_UNUSED(blobsizes);
    INDI_UNUSED(blobs);
    INDI_UNUSED(formats);
    INDI_UNUSED(names);
    INDI_UNUSED(n);
}

void ISSnoopDevice(XMLEle *root)
{
    scope->ISSnoopDevice(root);
}

/* Constructor */
PMC8::PMC8()
{
    currentRA  = ln_get_apparent_sidereal_time(ln_get_julian_from_sys());
    currentDEC = 90;

    DBG_SCOPE = INDI::Logger::getInstance().addDebugLevel("Scope Verbose", "SCOPE");

    SetTelescopeCapability(TELESCOPE_CAN_PARK | TELESCOPE_CAN_SYNC | TELESCOPE_CAN_GOTO | TELESCOPE_CAN_ABORT |
                           TELESCOPE_HAS_TRACK_MODE | TELESCOPE_CAN_CONTROL_TRACK | TELESCOPE_HAS_TRACK_RATE |
                           TELESCOPE_HAS_LOCATION,
                           4);

    setVersion(0, 3);
}

PMC8::~PMC8()
{
}

const char *PMC8::getDefaultName()
{
    return "PMC8";
}

bool PMC8::initProperties()
{
    INDI::Telescope::initProperties();

    // My understanding is that all mounts communicate at 115200
    serialConnection->setDefaultBaudRate(Connection::Serial::B_115200);

    tcpConnection->setDefaultHost(PMC8_DEFAULT_IP_ADDRESS);
    tcpConnection->setDefaultPort(PMC8_DEFAULT_PORT);

    // Mount Type
    IUFillSwitch(&MountTypeS[MOUNT_G11], "MOUNT_G11", "G11", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_EXOS2], "MOUNT_EXOS2", "EXOS2", ISS_OFF);
    IUFillSwitch(&MountTypeS[MOUNT_iEXOS100], "MOUNT_iEXOS100", "iEXOS100", ISS_OFF);
    IUFillSwitchVector(&MountTypeSP, MountTypeS, 3, getDeviceName(), "MOUNT_TYPE", "Mount Type", CONNECTION_TAB,
                       IP_RW, ISR_1OFMANY, 0, IPS_IDLE);

    /* Tracking Mode */
    // order is important, since driver assumes solar = 1, lunar = 2
    AddTrackMode("TRACK_SIDEREAL", "Sidereal", true);
    AddTrackMode("TRACK_SOLAR", "Solar");
    AddTrackMode("TRACK_LUNAR", "Lunar");
    //AddTrackMode("TRACK_KING", "King"); // King appears to be effectively the same as Solar, at least for EXOS-2, and a bit of pain to implement with auto-detection
    AddTrackMode("TRACK_CUSTOM", "Custom");

    // Set TrackRate limits
    /*TrackRateN[AXIS_RA].min = -PMC8_MAX_TRACK_RATE;
    TrackRateN[AXIS_RA].max = PMC8_MAX_TRACK_RATE;
    TrackRateN[AXIS_DE].min = -0.01;
    TrackRateN[AXIS_DE].max = 0.01;*/

    // relabel move speeds
    strcpy(SlewRateSP.sp[0].label, "4x");
    strcpy(SlewRateSP.sp[1].label, "16x");
    strcpy(SlewRateSP.sp[2].label, "64x");
    strcpy(SlewRateSP.sp[3].label, "256x");

    /* How fast do we guide compared to sidereal rate */
    IUFillNumber(&RaGuideRateN[0], "GUIDE_RATE", "x Sidereal", "%g", 0.1, 1.0, 0.1, 0.4);
    IUFillNumberVector(&RaGuideRateNP, RaGuideRateN, 1, getDeviceName(), "GUIDE_RATE", "RA Guiding Rate", MOTION_TAB, IP_RW, 0,
                       IPS_IDLE);
    IUFillNumber(&DeGuideRateN[0], "GUIDE_RATE_DE", "x Sidereal", "%g", 0.1, 1.0, 0.1, 0.4);
    IUFillNumberVector(&DeGuideRateNP, DeGuideRateN, 1, getDeviceName(), "GUIDE_RATE_DE", "DEC Guiding Rate", MOTION_TAB, IP_RW, 0,
                       IPS_IDLE);


    initGuiderProperties(getDeviceName(), MOTION_TAB);

    TrackState = SCOPE_IDLE;

    // Driver does not support custom parking yet.
    SetParkDataType(PARK_NONE);

    addAuxControls();

    set_pmc8_device(getDeviceName());

    IUFillText(&FirmwareT[0], "Version", "Version", "");
    IUFillTextVector(&FirmwareTP, FirmwareT, 1, getDeviceName(), "Firmware", "Firmware", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

    setDriverInterface(getDriverInterface() | GUIDER_INTERFACE);

    return true;
}

bool PMC8::updateProperties()
{
    INDI::Telescope::updateProperties();

    if (isConnected())
    {
        defineProperty(&GuideNSNP);
        defineProperty(&GuideWENP);
        defineProperty(&RaGuideRateNP);
        defineProperty(&DeGuideRateNP);

        defineProperty(&FirmwareTP);

        // do not support park position
        deleteProperty(ParkPositionNP.name);
        deleteProperty(ParkOptionSP.name);

        getStartupData();
    }
    else
    {
        deleteProperty(GuideNSNP.name);
        deleteProperty(GuideWENP.name);
        deleteProperty(RaGuideRateNP.name);
        deleteProperty(DeGuideRateNP.name);

        deleteProperty(FirmwareTP.name);
    }

    return true;
}

void PMC8::getStartupData()
{
    LOG_DEBUG("Getting firmware data...");
    if (get_pmc8_firmware(PortFD, &firmwareInfo))
    {
        const char *c;

        // FIXME - Need to add code to get firmware data
        FirmwareTP.s = IPS_OK;
        c = firmwareInfo.MainBoardFirmware.c_str();
        LOGF_INFO("firmware = %s.", c);

        // not sure if there's really a point to the mount switch anymore if we know the mount from the firmware - perhaps remove as newer firmware becomes standard?
        // populate mount type switch in interface from firmware if possible
        if (firmwareInfo.MountType == MOUNT_EXOS2) {
            MountTypeS[MOUNT_EXOS2].s = ISS_ON;
            LOG_INFO("Detected mount type as Exos2.");
        }
        else if (firmwareInfo.MountType == MOUNT_G11) {
            MountTypeS[MOUNT_G11].s = ISS_ON;
            LOG_INFO("Detected mount type as G11.");
        }
        else if (firmwareInfo.MountType == MOUNT_iEXOS100) {
            MountTypeS[MOUNT_iEXOS100].s = ISS_ON;
            LOG_INFO("Detected mount type as iExos100.");
        }
        else {
            LOG_INFO("Cannot detect mount type--perhaps this is older firmware?");
            if (strstr(getDeviceName(), "EXOS2")) {
                MountTypeS[MOUNT_EXOS2].s = ISS_ON;
                LOG_INFO("Guessing mount is EXOS2 from device name.");
            }
            else if (strstr(getDeviceName(), "iEXOS100")) {
                MountTypeS[MOUNT_iEXOS100].s = ISS_ON;
                LOG_INFO("Guessing mount is iEXOS100 from device name.");
            }
            else {
                MountTypeS[MOUNT_G11].s = ISS_ON;
                LOG_INFO("Guessing mount is G11.");
            }
        }
        MountTypeSP.s = IPS_OK;
        IDSetSwitch(&MountTypeSP,nullptr);

        IUSaveText(&FirmwareT[0], c);
        IDSetText(&FirmwareTP, nullptr);
    }
        
    // get SRF values
    double rate = 0.4;
    if (get_pmc8_guide_rate(PortFD,PMC8_AXIS_RA,rate)) {
        RaGuideRateN[0].value = rate;
        RaGuideRateNP.s = IPS_OK;
        IDSetNumber(&RaGuideRateNP, nullptr);
    }
    if (get_pmc8_guide_rate(PortFD,PMC8_AXIS_DEC,rate)) {
        DeGuideRateN[0].value = rate;
        DeGuideRateNP.s = IPS_OK;
        IDSetNumber(&DeGuideRateNP, nullptr);
    }
            
    // PMC8 doesn't store location permanently so read from config and set
    // Convert to INDI standard longitude (0 to 360 Eastward)
    double longitude = LocationN[LOCATION_LONGITUDE].value;
    double latitude = LocationN[LOCATION_LATITUDE].value;

    // must also keep "low level" aware of position to convert motor counts to RA/DEC
    set_pmc8_location(latitude, longitude);

    // seems like best place to put a warning that will be seen in log window of EKOS/etc
    LOG_INFO("The PMC-Eight driver is in BETA development currently.");
    LOG_INFO("Be prepared to intervene if something unexpected occurs.");

#if 0
    // FIXEME - Need to handle southern hemisphere for DEC?
    double HA  = ln_get_apparent_sidereal_time(ln_get_julian_from_sys());
    double DEC = 90;

    // currently only park at motor position (0, 0)
    if (InitPark())
    {
        // If loading parking data is successful, we just set the default parking values.
        SetAxis1ParkDefault(HA);
        SetAxis2ParkDefault(DEC);
    }
    else
    {
        // Otherwise, we set all parking data to default in case no parking data is found.
        SetAxis1Park(HA);
        SetAxis2Park(DEC);
        SetAxis1ParkDefault(HA);
        SetAxis2ParkDefault(DEC);
    }
#endif

#if 0
    // FIXME - Need to implement simulation functionality
    if (isSimulation())
    {
        if (isParked())
            set_sim_system_status(ST_PARKED);
        else
            set_sim_system_status(ST_STOPPED);
    }
#endif
}

bool PMC8::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    if (!strcmp(dev, getDeviceName()))
    {
        // Guiding Rate
        if (!strcmp(name, RaGuideRateNP.name))
        {
            IUUpdateNumber(&RaGuideRateNP, values, names, n);

            if (set_pmc8_guide_rate(PortFD, PMC8_AXIS_RA, RaGuideRateN[0].value))
                RaGuideRateNP.s = IPS_OK;
            else
                RaGuideRateNP.s = IPS_ALERT;

            IDSetNumber(&RaGuideRateNP, nullptr);

            return true;
        }
        if (!strcmp(name, DeGuideRateNP.name))
        {
            IUUpdateNumber(&DeGuideRateNP, values, names, n);

            if (set_pmc8_guide_rate(PortFD, PMC8_AXIS_DEC, DeGuideRateN[0].value))
                DeGuideRateNP.s = IPS_OK;
            else
                DeGuideRateNP.s = IPS_ALERT;

            IDSetNumber(&DeGuideRateNP, nullptr);

            return true;
        }


        if (!strcmp(name, GuideNSNP.name) || !strcmp(name, GuideWENP.name))
        {
            processGuiderProperties(name, values, names, n);
            return true;
        }
    }

    return INDI::Telescope::ISNewNumber(dev, name, values, names, n);
}

void PMC8::ISGetProperties(const char *dev)
{
    INDI::Telescope::ISGetProperties(dev);
    defineProperty(&MountTypeSP);
}

bool PMC8::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        if (strcmp(name, MountTypeSP.name) == 0)
        {
            IUUpdateSwitch(&MountTypeSP, states, names, n);
            int currentMountIndex = IUFindOnSwitchIndex(&MountTypeSP);
            LOGF_INFO("Selected mount is %s", MountTypeS[currentMountIndex].label);

            //right now, this lets the user override the parameters for the detected mount.  Perhaps we should prevent the user from doing so?
            set_pmc8_mountParameters(currentMountIndex);
            MountTypeSP.s = IPS_OK;
            IDSetSwitch(&MountTypeSP, nullptr);
            //		defineProperty(&MountTypeSP);
            return true;
        }
    }

    return INDI::Telescope::ISNewSwitch(dev, name, states, names, n);
}

bool PMC8::ReadScopeStatus()
{
    bool rc = false;

    // try to disconnect and reconnect if reconnect flag is set
    if (get_pmc8_reconnect_flag()) {
        int rc = Disconnect();
        if (rc) setConnected(false);
        rc = Connect();
        if (rc) setConnected(true, IPS_OK);        
        return false;
    }

    if (isSimulation())
        mountSim();

    // avoid unnecessary status calls to mount while pulse guiding so we don't lock up the mount for 40+ ms right when it needs to start/stop
    if (isPulsingNS || isPulsingWE) return true;

    bool slewing = false;

    switch (TrackState)
    {
        case SCOPE_SLEWING:
            // are we done?
            // check slew state
            rc = get_pmc8_is_scope_slewing(PortFD, slewing);
            if (!rc)
            {
                LOG_ERROR("PMC8::ReadScopeStatus() - unable to check slew state");
            }
            else
            {
                if (slewing == false)
                {
                    LOG_INFO("Slew complete, tracking...");
                    TrackState = SCOPE_TRACKING;
                    // Technically, we don't need to restart tracking with v2 firmware, but it doesn't hurt
                    if (!SetTrackEnabled(true))
                    {
                        LOG_ERROR("slew complete - unable to enable tracking");
                        return false;
                    }

                    // Already set track mode in SetTrackEnabled 
                    //if (!SetTrackMode(IUFindOnSwitchIndex(&TrackModeSP)))
                    //{
                    //    LOG_ERROR("slew complete - unable to set track mode");
                    //    return false;
                    //}
                }
            }

            break;

        case SCOPE_PARKING:
            // are we done?
            // check slew state
            rc = get_pmc8_is_scope_slewing(PortFD, slewing);
            if (!rc)
            {
                LOG_ERROR("PMC8::ReadScopeStatus() - unable to check slew state");
            }
            else
            {
                if (slewing == false)
                {
                    if (stop_pmc8_tracking_motion(PortFD))
                        LOG_DEBUG("Mount tracking is off.");

                    SetParked(true);

                    saveConfig(true);
                }
            }
            break;
            
        case SCOPE_IDLE:
            //periodically check to see if we've entered tracking state (e.g. at startup or from other client)
            if (!trackingPollCounter--) {
                
                trackingPollCounter = PMC8_TRACKING_AUTODETECT_INTERVAL;
                
                double track_rate;
                uint8_t track_mode;
                
                rc = get_pmc8_tracking_data(PortFD,track_rate,track_mode);
                
                if (rc && ((int)track_rate>0) && ((int)track_rate<=PMC8_MAX_TRACK_RATE))
                {
                    IUResetSwitch(&TrackModeSP);
                    TrackModeS[convertFromPMC8TrackMode(track_mode)].s = ISS_ON;   
                    IDSetSwitch(&TrackModeSP, nullptr);             
                    TrackState = SCOPE_TRACKING;
                    TrackRateNP.s           = IPS_IDLE;
                    TrackRateN[AXIS_RA].value = track_rate;
                    IDSetNumber(&TrackRateNP, nullptr);
                    DEBUGF(INDI::Logger::DBG_DEBUG, "Mount tracking at %f arcsec / sec", track_rate);
                }
            }
            break;
            
        case SCOPE_TRACKING:
           //periodically check to see if we've stopped tracking or changed speed (e.g. from other client)
            if (!trackingPollCounter--) {
                trackingPollCounter = PMC8_TRACKING_AUTODETECT_INTERVAL;

                double track_rate;
                uint8_t track_mode;
                
                rc = get_pmc8_tracking_data(PortFD,track_rate,track_mode);

                if (rc && ((int)track_rate==0))
                {
                    DEBUG(INDI::Logger::DBG_SESSION, "Mount appears to have stopped tracking");
                    TrackState = SCOPE_IDLE;
                }                
                else if (rc && ((int)track_rate<=PMC8_MAX_TRACK_RATE)) {
                    if (TrackModeS[convertFromPMC8TrackMode(track_mode)].s != ISS_ON) {
                        IUResetSwitch(&TrackModeSP);
                        TrackModeS[convertFromPMC8TrackMode(track_mode)].s = ISS_ON;
                        IDSetSwitch(&TrackModeSP, nullptr);             
                    }
                    if (TrackRateN[AXIS_RA].value != track_rate) {                     
                        TrackState = SCOPE_TRACKING;
                        TrackRateNP.s           = IPS_IDLE;
                        TrackRateN[AXIS_RA].value = track_rate;
                        IDSetNumber(&TrackRateNP, nullptr);
                    }
                    DEBUGF(INDI::Logger::DBG_DEBUG, "Mount tracking at %f arcsec / sec", track_rate);
                }
            }        

        default:
            break;
    }

    rc = get_pmc8_coords(PortFD, currentRA, currentDEC);

    if (rc)
        NewRaDec(currentRA, currentDEC);

    return rc;
}

bool PMC8::Goto(double r, double d)
{
    char RAStr[64] = {0}, DecStr[64] = {0};

    targetRA  = r;
    targetDEC = d;

    fs_sexa(RAStr, targetRA, 2, 3600);
    fs_sexa(DecStr, targetDEC, 2, 3600);

    DEBUGF(INDI::Logger::DBG_SESSION, "Slewing to RA: %s - DEC: %s", RAStr, DecStr);


    if (slew_pmc8(PortFD, r, d) == false)
    {
        LOG_ERROR("Failed to slew.");
        return false;
    }

    TrackState = SCOPE_SLEWING;

    return true;
}

bool PMC8::Sync(double ra, double dec)
{

    targetRA  = ra;
    targetDEC = dec;
    char RAStr[64] = {0}, DecStr[64] = {0};

    fs_sexa(RAStr, targetRA, 2, 3600);
    fs_sexa(DecStr, targetDEC, 2, 3600);

    DEBUGF(INDI::Logger::DBG_SESSION, "Syncing to RA: %s - DEC: %s", RAStr, DecStr);

    if (sync_pmc8(PortFD, ra, dec) == false)
    {
        LOG_ERROR("Failed to sync.");
    }

    EqNP.s     = IPS_OK;

    currentRA  = ra;
    currentDEC = dec;

    NewRaDec(currentRA, currentDEC);

    return true;
}

bool PMC8::Abort()
{

    //GUIDE Abort guide operations.
    if (GuideNSNP.s == IPS_BUSY || GuideWENP.s == IPS_BUSY)
    {
        GuideNSNP.s = GuideWENP.s = IPS_IDLE;
        GuideNSN[0].value = GuideNSN[1].value = 0.0;
        GuideWEN[0].value = GuideWEN[1].value = 0.0;

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideNSTID = 0;
        }

        LOG_INFO("Guide aborted.");
        IDSetNumber(&GuideNSNP, nullptr);
        IDSetNumber(&GuideWENP, nullptr);

        return true;
    }

    return abort_pmc8(PortFD);
}

bool PMC8::Park()
{
#if 0
    // FIXME - Currently only support parking at motor position (0, 0)
    targetRA  = GetAxis1Park();
    targetDEC = GetAxis2Park();
    if (set_pmc8_radec(PortFD, r, d) == false)
    {
        LOG_ERROR("Error setting RA/DEC.");
        return false;
    }
#endif

    if (park_pmc8(PortFD))
    {
        TrackState = SCOPE_PARKING;
        LOG_INFO("Telescope parking in progress to motor position (0, 0)");
        return true;
    }
    else
    {
        return false;
    }
}

bool PMC8::UnPark()
{
    if (unpark_pmc8(PortFD))
    {
        SetParked(false);
        TrackState = SCOPE_IDLE;
        return true;
    }
    else
    {
        return false;
    }
}

bool PMC8::Handshake()
{
    if (isSimulation())
    {
        set_pmc8_sim_system_status(ST_STOPPED);
        set_pmc8_sim_track_rate(PMC8_TRACK_SIDEREAL);
        set_pmc8_sim_move_rate(PMC8_MOVE_64X);
        //        set_pmc8_sim_hemisphere(HEMI_NORTH);
    }

    if (check_pmc8_connection(PortFD,(getActiveConnection() == serialConnection)) == false)
        return false;

    return true;
}

bool PMC8::updateTime(ln_date *utc, double utc_offset)
{
    // mark unused
    INDI_UNUSED(utc);
    INDI_UNUSED(utc_offset);

    LOG_ERROR("PMC8::updateTime() not implemented!");
    return false;

}

bool PMC8::updateLocation(double latitude, double longitude, double elevation)
{
    INDI_UNUSED(elevation);

    if (longitude > 180)
        longitude -= 360;

    // experimental support for Southern Hemisphere!
    if (latitude < 0)
    {
        LOG_WARN("Southern Hemisphere support still experimental!");
        //return false;
    }

    // must also keep "low level" aware of position to convert motor counts to RA/DEC
    set_pmc8_location(latitude, longitude);

    char l[32] = {0}, L[32] = {0};
    fs_sexa(l, latitude, 3, 3600);
    fs_sexa(L, longitude, 4, 3600);

    LOGF_INFO("Site location updated to Lat %.32s - Long %.32s", l, L);

    return true;
}

void PMC8::debugTriggered(bool enable)
{
    set_pmc8_debug(enable);
}

void PMC8::simulationTriggered(bool enable)
{
    set_pmc8_simulation(enable);
}

bool PMC8::MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command)
{
    if (TrackState == SCOPE_PARKED)
    {
        LOG_ERROR("Please unpark the mount before issuing any motion commands.");
        return false;
    }

    // read desired move rate
    int currentIndex = IUFindOnSwitchIndex(&SlewRateSP);

    LOGF_DEBUG("MoveNS at slew index %d", currentIndex);

    switch (command)
    {
        case MOTION_START:
            if (start_pmc8_motion(PortFD, (dir == DIRECTION_NORTH ? PMC8_N : PMC8_S), currentIndex) == false)
            {
                LOG_ERROR("Error setting N/S motion direction.");
                return false;
            }
            else
            {
                LOGF_INFO("Moving toward %s.", (dir == DIRECTION_NORTH) ? "North" : "South");
            }
            break;

        case MOTION_STOP:
            if (stop_pmc8_motion(PortFD, (dir == DIRECTION_NORTH ? PMC8_N : PMC8_S)) == false)
            {
                LOG_ERROR("Error stopping N/S motion.");
                return false;
            }
            else
            {
                LOGF_INFO("%s motion stopped.", (dir == DIRECTION_NORTH) ? "North" : "South");
            }
            break;
    }

    return true;
}

bool PMC8::MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command)
{
    if (TrackState == SCOPE_PARKED)
    {
        LOG_ERROR("Please unpark the mount before issuing any motion commands.");
        return false;
    }

    // read desired move rate
    int currentIndex = IUFindOnSwitchIndex(&SlewRateSP);

    LOGF_DEBUG("MoveWE at slew index %d", currentIndex);

    switch (command)
    {
        case MOTION_START:
            if (start_pmc8_motion(PortFD, (dir == DIRECTION_WEST ? PMC8_W : PMC8_E), currentIndex) == false)
            {
                LOG_ERROR("Error setting N/S motion direction.");
                return false;
            }
            else
            {
                LOGF_INFO("Moving toward %s.", (dir == DIRECTION_WEST) ? "West" : "East");
            }
            break;

        case MOTION_STOP:
            if (stop_pmc8_motion(PortFD, (dir == DIRECTION_WEST ? PMC8_W : PMC8_E)) == false)
            {
                LOG_ERROR("Error stopping W/E motion.");
                return false;
            }
            else
            {
                LOGF_INFO("%s motion stopped.", (dir == DIRECTION_WEST) ? "West" : "East");

                // restore tracking

                if (TrackState == SCOPE_TRACKING)
                {
                    LOG_INFO("Move E/W complete, tracking...");

                    if (!SetTrackEnabled(true))
                    {
                        LOG_ERROR("slew complete - unable to enable tracking");
                        return false;
                    }

                    if (!SetTrackMode(IUFindOnSwitchIndex(&TrackModeSP)))
                    {
                        LOG_ERROR("slew complete - unable to set track mode");
                        return false;
                    }
                }
            }
            break;
    }

    return true;
}

IPState PMC8::GuideNorth(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;
    
    //only guide if tracking
    if (TrackState == SCOPE_TRACKING) {

        // If already moving, then stop movement
        if (MovementNSSP.s == IPS_BUSY)
        {
            int dir = IUFindOnSwitchIndex(&MovementNSSP);
            MoveNS(dir == 0 ? DIRECTION_NORTH : DIRECTION_SOUTH, MOTION_STOP);
        }

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        isPulsingNS = true;
        start_pmc8_guide(PortFD, PMC8_N, (int)ms, timetaken_us, 0);

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideNSTID      = IEAddTimer(timeremain_ms, guideTimeoutHelperN, this);
    return ret;
}

IPState PMC8::GuideSouth(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;
    
    //only guide if tracking
    if (TrackState == SCOPE_TRACKING) {

        // If already moving, then stop movement
        if (MovementNSSP.s == IPS_BUSY)
        {
            int dir = IUFindOnSwitchIndex(&MovementNSSP);
            MoveNS(dir == 0 ? DIRECTION_NORTH : DIRECTION_SOUTH, MOTION_STOP);
        }

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        isPulsingNS = true;
        start_pmc8_guide(PortFD, PMC8_S, (int)ms, timetaken_us, 0);

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideNSTID      = IEAddTimer(timeremain_ms, guideTimeoutHelperS, this);
    return ret;
}

IPState PMC8::GuideEast(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;
    
    //only guide if tracking
    if (TrackState == SCOPE_TRACKING) {

        // If already moving (no pulse command), then stop movement
        if (MovementWESP.s == IPS_BUSY)
        {
            int dir = IUFindOnSwitchIndex(&MovementWESP);
            MoveWE(dir == 0 ? DIRECTION_WEST : DIRECTION_EAST, MOTION_STOP);
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideWETID = 0;
        }

        isPulsingWE = true;
        start_pmc8_guide(PortFD, PMC8_E, (int)ms, timetaken_us, TrackRateN[AXIS_RA].value);

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideWETID      = IEAddTimer(timeremain_ms, guideTimeoutHelperE, this);
    return ret;
}

IPState PMC8::GuideWest(uint32_t ms)
{
    IPState ret = IPS_IDLE;
    long timetaken_us = 0;
    int timeremain_ms = 0;
    
    //only guide if tracking
    if (TrackState == SCOPE_TRACKING) {

        // If already moving (no pulse command), then stop movement
        if (MovementWESP.s == IPS_BUSY)
        {
            int dir = IUFindOnSwitchIndex(&MovementWESP);
            MoveWE(dir == 0 ? DIRECTION_WEST : DIRECTION_EAST, MOTION_STOP);
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideWETID = 0;
        }

        isPulsingWE = true;
        start_pmc8_guide(PortFD, PMC8_W, (int)ms, timetaken_us, TrackRateN[AXIS_RA].value);

        timeremain_ms = (int)(ms - ((float)timetaken_us) / 1000.0);

        if (timeremain_ms < 0)
            timeremain_ms = 0;

        ret = IPS_BUSY;
    }
    else {
        LOG_INFO("Mount not tracking--cannot guide.");
    }
    GuideWETID      = IEAddTimer(timeremain_ms, guideTimeoutHelperW, this);
    return ret;
}

void PMC8::guideTimeout(PMC8_DIRECTION calldir)
{
    // end previous pulse command
    stop_pmc8_guide(PortFD, calldir);
    
    if (calldir == PMC8_N || calldir == PMC8_S)
    {
        isPulsingNS = false;
        GuideNSNP.np[0].value = 0;
        GuideNSNP.np[1].value = 0;
        GuideNSNP.s           = IPS_IDLE;
        GuideNSTID            = 0;
        IDSetNumber(&GuideNSNP, nullptr);
    }
    if (calldir == PMC8_W || calldir == PMC8_E)
    {
        isPulsingWE = false;
        GuideWENP.np[0].value = 0;
        GuideWENP.np[1].value = 0;
        GuideWENP.s           = IPS_IDLE;
        GuideWETID            = 0;
        IDSetNumber(&GuideWENP, nullptr);
    }

    LOG_DEBUG("GUIDE CMD COMPLETED");
}

//GUIDE The timer helper functions.
void PMC8::guideTimeoutHelperN(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_N);
}
void PMC8::guideTimeoutHelperS(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_S);
}
void PMC8::guideTimeoutHelperW(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_W);
}
void PMC8::guideTimeoutHelperE(void *p)
{
    static_cast<PMC8*>(p)->guideTimeout(PMC8_E);
}

bool PMC8::SetSlewRate(int index)
{

    INDI_UNUSED(index);

    // slew rate is rate for MoveEW/MOVENE commands - not for GOTOs!!!

    // just return true - we will check SlewRateSP when we do actually moves
    return true;
}

bool PMC8::saveConfigItems(FILE *fp)
{
    INDI::Telescope::saveConfigItems(fp);

    IUSaveConfigSwitch(fp, &MountTypeSP);
    return true;
}

void PMC8::mountSim()
{
    static struct timeval ltv;
    struct timeval tv;
    double dt, da, dx;
    int nlocked;

    /* update elapsed time since last poll, don't presume exactly POLLMS */
    gettimeofday(&tv, nullptr);

    if (ltv.tv_sec == 0 && ltv.tv_usec == 0)
        ltv = tv;

    dt  = tv.tv_sec - ltv.tv_sec + (tv.tv_usec - ltv.tv_usec) / 1e6;
    ltv = tv;
    da  = SLEWRATE * dt;

    /* Process per current state. We check the state of EQUATORIAL_COORDS and act acoordingly */
    switch (TrackState)
    {
        case SCOPE_IDLE:
            currentRA += (TrackRateN[AXIS_RA].value / 3600.0 * dt) / 15.0;
            currentRA = range24(currentRA);
            break;

        case SCOPE_TRACKING:
            if (TrackModeS[1].s == ISS_ON)
            {
                currentRA  += ( ((TRACKRATE_SIDEREAL / 3600.0) - (TrackRateN[AXIS_RA].value / 3600.0)) * dt) / 15.0;
                currentDEC += ( (TrackRateN[AXIS_DE].value / 3600.0) * dt);
            }
            break;

        case SCOPE_SLEWING:
        case SCOPE_PARKING:
            /* slewing - nail it when both within one pulse @ SLEWRATE */
            nlocked = 0;

            dx = targetRA - currentRA;

            // Take shortest path
            if (fabs(dx) > 12)
                dx *= -1;

            if (fabs(dx) <= da)
            {
                currentRA = targetRA;
                nlocked++;
            }
            else if (dx > 0)
                currentRA += da / 15.;
            else
                currentRA -= da / 15.;

            if (currentRA < 0)
                currentRA += 24;
            else if (currentRA > 24)
                currentRA -= 24;

            dx = targetDEC - currentDEC;
            if (fabs(dx) <= da)
            {
                currentDEC = targetDEC;
                nlocked++;
            }
            else if (dx > 0)
                currentDEC += da;
            else
                currentDEC -= da;

            if (nlocked == 2)
            {
                if (TrackState == SCOPE_SLEWING)
                    set_pmc8_sim_system_status(ST_TRACKING);
                else
                    set_pmc8_sim_system_status(ST_PARKED);
            }

            break;

        case SCOPE_PARKED:
            // setting system status to parked will automatically
            // set the simulated RA/DEC to park position so reread
            set_pmc8_sim_system_status(ST_PARKED);
            get_pmc8_coords(PortFD, currentRA, currentDEC);

            break;

        default:
            break;
    }

    set_pmc8_sim_ra(currentRA);
    set_pmc8_sim_dec(currentDEC);
}

#if 0
// PMC8 only parks to motor position (0, 0) currently
bool PMC8::SetCurrentPark()
{
    SetAxis1Park(currentRA);
    SetAxis2Park(currentDEC);

    return true;
}

bool PMC8::SetDefaultPark()
{
    // By default set RA to HA
    SetAxis1Park(ln_get_apparent_sidereal_time(ln_get_julian_from_sys()));

    // Set DEC to 90 or -90 depending on the hemisphere
    //    SetAxis2Park((HemisphereS[HEMI_NORTH].s == ISS_ON) ? 90 : -90);
    SetAxis2Park(90);

    return true;
}
#else
bool PMC8::SetCurrentPark()
{
    LOG_ERROR("PPMC8::SetCurrentPark() not implemented!");
    return false;
}

bool PMC8::SetDefaultPark()
{
    LOG_ERROR("PMC8::SetDefaultPark() not implemented!");
    return false;
}
#endif

uint8_t PMC8::convertToPMC8TrackMode(uint8_t mode) {
    switch (mode)
    {
        case TRACK_SIDEREAL:
            return PMC8_TRACK_SIDEREAL;
            break;
        case TRACK_LUNAR:
            return PMC8_TRACK_LUNAR;
            break;
        case TRACK_SOLAR:
            return PMC8_TRACK_SOLAR;
            break;
        case TRACK_CUSTOM:
            return PMC8_TRACK_CUSTOM;
            break;
        default:
            return PMC8_TRACK_UNDEFINED;
    }
}

uint8_t PMC8::convertFromPMC8TrackMode(uint8_t mode) {
    switch (mode)
    {
        case PMC8_TRACK_SIDEREAL:
            return TRACK_SIDEREAL;
            break;
        case PMC8_TRACK_LUNAR:
            return TRACK_LUNAR;
            break;
        case PMC8_TRACK_SOLAR:
            return TRACK_SOLAR;
            break;
        default:
            return TRACK_CUSTOM;
    }
}

bool PMC8::SetTrackMode(uint8_t mode)
{
    uint8_t pmc8_mode;

    LOGF_DEBUG("PMC8::SetTrackMode called mode=%d", mode);

    // FIXME - Need to make sure track modes are handled properly!
    //PMC8_TRACK_RATE rate = static_cast<PMC8_TRACK_RATE>(mode);
    // not sure what needs fixing

    pmc8_mode = convertToPMC8TrackMode(mode);
    
    if (pmc8_mode == PMC8_TRACK_UNDEFINED) {
            LOGF_ERROR("PMC8::SetTrackMode mode=%d not supported!", mode);
            return false;
    }

    if (pmc8_mode == PMC8_TRACK_CUSTOM)
    {
        if (set_pmc8_ra_tracking(PortFD, TrackRateN[AXIS_RA].value))
            return true;
    }
    else
    {
        if (set_pmc8_track_mode(PortFD, pmc8_mode))
            return true;
    }

    return false;
}

bool PMC8::SetTrackRate(double raRate, double deRate)
{
    static bool deRateWarning = true;
    double pmc8RARate;

    LOGF_DEBUG("PMC8::SetTrackRate called raRate=%f  deRate=%f", raRate, deRate);

    // Convert to arcsecs/s to +/- 0.0100 accepted by
    //double pmc8RARate = raRate - TRACKRATE_SIDEREAL;

    // for now just send rate
    pmc8RARate = raRate;

    if (deRate != 0 && deRateWarning)
    {
        // Only send warning once per session
        deRateWarning = false;
        LOG_WARN("Custom Declination tracking rate is not implemented yet.");
    }

    if (set_pmc8_ra_tracking(PortFD, pmc8RARate))
        return true;

    LOG_ERROR("PMC8::SetTrackRate not implemented!");
    return false;
}

bool PMC8::SetTrackEnabled(bool enabled)
{

    LOGF_DEBUG("PMC8::SetTrackEnabled called enabled=%d", enabled);

    // need to determine current tracking mode and start tracking
    if (enabled)
    {

        if (!SetTrackMode(IUFindOnSwitchIndex(&TrackModeSP)))
        {
            LOG_ERROR("PMC8::SetTrackEnabled - unable to enable tracking");
            return false;
        }
    }
    else
    {
        bool rc;

        rc = set_pmc8_custom_ra_track_rate(PortFD, 0);
        if (!rc)
        {
            LOG_ERROR("PMC8::SetTrackEnabled - unable to set RA track rate to 0");
            return false;
        }

        // currently only support tracking rate in RA
        //        rc=set_pmc8_custom_dec_track_rate(PortFD, 0);
        //        if (!rc)
        //        {
        //            LOG_ERROR("PMC8::SetTrackREnabled - unable to set DEC track rate to 0");
        //            return false;
        //        }
    }

    return true;
}

