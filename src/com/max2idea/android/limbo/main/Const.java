/*
Copyright (C) Max Kastanas 2012

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
package com.max2idea.android.limbo.main;

import android.os.Environment;

/**
 *
 * @author dev
 */
public class Const {

	public static boolean debug = false;
	
    public static String APP_NAME = "Limbo PC Emulator (QEMU)";
    public static String basefiledir = Environment.getExternalStorageDirectory() + "/limbo/";
    public static String machinedir = basefiledir + "/machines/";
    public static int SETTINGS_RETURN_CODE = 1000;
    public static int SETTINGS_REQUEST_CODE = 1001;
    public static int FILEMAN_RETURN_CODE=1002;
    public static int FILEMAN_REQUEST_CODE=1003;
    public static int VNC_REQUEST_CODE=1004;
    public static int VNC_RESULT_CODE=1005;
    public static int VNC_RESET_RESULT_CODE=1006;
    
    public static int STATUS_NULL = -1;
    public static int STATUS_CREATED = 1000;
    public static int STATUS_PAUSED = 1001;
    
    static int VM_CREATED = 1002;
    static int VM_STARTED = 1003;
    static int VM_STOPPED = 1004;
    static int VM_NOTRUNNING = 1005;
    static int VM_RESTARTED = 1006;
    static int VM_PAUSED = 1007;
    static int VM_RESUMED = 1008;
    static int VM_SAVED = 1009;
    static int IMG_CREATED = 1010;
    static int VNC_PASSWORD = 1011;
    public static int SNAPSHOT_CREATED = 1012;
    public static int UIUTILS_SHOWALERT_HTML = 1013;    
    public static int VM_NO_QCOW2 = 1014;
    public static int STATUS_CHANGED = 1015;
    public static int UIUTILS_SHOWALERT_HELP = 1016;
    public static boolean NOT_ICS = true;
	public static String dnsServer = "8.8.8.8";
    
    
    
    

    

}
