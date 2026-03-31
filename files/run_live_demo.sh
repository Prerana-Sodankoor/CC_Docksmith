#!/bin/bash
set -e

echo -e "\n\033[1;36m====================================================\033[0m"
echo -e "\033[1;35m      DOCKSMITH COMPLETE LIVE DEMO (SECTION 9)      \033[0m"
echo -e "\033[1;36m====================================================\033[0m\n"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
  echo "Please run this script with sudo!"
  exit 1
fi

echo -e "\033[1;33m[PRE-REQUISITE] Downloading Alpine base offline...\033[0m"
bash setup_alpine.sh >/dev/null 2>&1
echo -e "\033[1;32mDone.\033[0m\n"

echo -e "\033[1;33m[PRE-REQUISITE] Compiling Engine...\033[0m"
make clean >/dev/null 2>&1 && make >/dev/null 2>&1
echo -e "\033[1;32mDone.\033[0m\n"

echo -e "\033[1;33m[STEP 1] Running Cold Build (docksmith build -t demo_app:latest sample_app/)\033[0m"
echo "Expected: All layer-producing steps show [CACHE MISS]. Total time printed."
read -p "Press ENTER to continue..."
./docksmith build -t demo_app:latest sample_app/ 
echo ""

echo -e "\033[1;33m[STEP 2] Running Warm Build (docksmith build -t demo_app:latest sample_app/)\033[0m"
echo "Expected: All layer-producing steps show [CACHE HIT]. Total time near-instantly."
read -p "Press ENTER to continue..."
./docksmith build -t demo_app:latest sample_app/ 
echo ""

echo -e "\033[1;33m[STEP 3] Edit a source file, then rebuild\033[0m"
echo "Editing app.sh directly, then rebuilding..."
echo "echo 'Bonus line added!'" >> sample_app/app.sh
read -p "Press ENTER to continue..."
./docksmith build -t demo_app:latest sample_app/ 
echo ""

echo -e "\033[1;33m[STEP 4] List Images (docksmith images)\033[0m"
echo "Expected: Image listed with proper Name, Tag, ID, and Timestamp."
read -p "Press ENTER to continue..."
./docksmith images
echo ""

echo -e "\033[1;33m[STEP 5] Run App (docksmith run demo_app:latest)\033[0m"
echo "Expected: App accurately reads ENV default 'DefaultGuest', reads 'build_stamp.txt', and safely exits."
read -p "Press ENTER to continue..."
./docksmith run demo_app:latest
echo ""

echo -e "\033[1;33m[STEP 6] Run App w/ ENV Override (docksmith run -e USER_NAME=PROFESSOR demo_app:latest)\033[0m"
echo "Expected: App should now greet 'PROFESSOR'."
read -p "Press ENTER to continue..."
./docksmith run -e USER_NAME="PROFESSOR_GRADER" demo_app:latest
echo ""

echo -e "\033[1;33m[STEP 7] Verify Container Filesystem Isolation\033[0m"
echo "Writing a secret file inside the container namespace root..."
read -p "Press ENTER to continue..."
./docksmith run demo_app:latest "sh -c 'touch /SECRET_CONTAINER_FILE.txt'"
echo "Now checking the HOST filesystem for the secret file..."
if [ -f "/SECRET_CONTAINER_FILE.txt" ]; then
    echo -e "\033[1;31m[FAIL] The container leaked the file to the host!\033[0m"
else
    echo -e "\033[1;32m[PASS] Isolated appropriately. File does NOT exist on the HOST machine.\033[0m"
fi
echo ""

echo -e "\033[1;33m[STEP 8] Delete Image & Layers (docksmith rmi demo_app:latest)\033[0m"
echo "Expected: Engine successfully destroys the manifest JSON and the associated tar layers safely."
read -p "Press ENTER to continue..."
./docksmith rmi demo_app:latest
echo ""

echo -e "\033[1;36m====================================================\033[0m"
echo -e "\033[1;32m             LIVE DEMO COMPLETED!                   \033[0m"
echo -e "\033[1;36m====================================================\033[0m\n"
