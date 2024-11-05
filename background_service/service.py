import subprocess
import firebase_admin
from firebase_admin import credentials
from firebase_admin import db
import requests
from datetime import datetime

cred = credentials.Certificate('./serviceAccountKey.json')
firebase_admin.initialize_app(cred, {
    'databaseURL': 'xxx' # Insert database URL here
})

ngrok_process = None

def start_ngrok(local_ip):
    global ngrok_process

    if ngrok_process is None or ngrok_process.poll() is not None:
        ngrok_url = f"http://{local_ip}:80"
        print(f"Starting ngrok tunnel with URL: {ngrok_url}")

        ngrok_process = subprocess.Popen(['ngrok', 'http', ngrok_url])
        print("ngrok process started.")

        time.sleep(5)  
    else:
        print("ngrok is already running.")

def get_public_ip():
    try:
        public_ip_info = requests.get('http://127.0.0.1:4040/api/tunnels').json()
        public_ip = public_ip_info['tunnels'][0]['public_url']  
        print(f"Public IP retrieved: {public_ip}")
        return public_ip
    except Exception as e:
        print(f"Error retrieving public IP: {e}")
        return None
  
def listener(event):
    if event.data is False:  
        local_ip_ref = db.reference('/streamData/localIp')
        local_ip = local_ip_ref.get()
        print(f"Local IP retrieved: {local_ip}")

        start_ngrok(local_ip)

        public_ip = get_public_ip()
        if public_ip:
            public_ip_ref = db.reference('/streamData/publicIp')
            public_ip_ref.set(public_ip)

            is_initialized_ref = db.reference('/streamData/isInitialized')
            is_initialized_ref.set(True)
            print("isInitialized set to True.")

            last_updated_ref = db.reference('/streamData/lastUpdated')
            last_updated_ref.set(datetime.now().strftime("%Y-%m-%d %H:%M:%S"))

print('Starting service')
ref = db.reference('/streamData/isInitialized')
ref.listen(listener)

import time
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("Listener stopped.")
