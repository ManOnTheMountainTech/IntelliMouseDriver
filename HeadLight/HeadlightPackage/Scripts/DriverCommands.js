function InstallDriver() {
    var shell = new ActiveXObject("WScript.Shell");
    var result = shell.Run("certmgr.exe /add TailLight.cer /s /r localMachine root", 0, true);
    WScript.Echo("Result ", result);

    result = shell.Run("certmgr.exe /add TailLight.cer /s /r localMachine trustedpublisher", 0, true);
    WScript.Echo("Result ", result);

    result = shell.Run("PNPUTIL /add-driver TailLight.inf /install", 0, true);
    WScript.Echo("Result ", result);

    switch (result) {
        case 0:
        case 3010:
        case 1641:
            return 0;

        default:
            return -1;
    }
}