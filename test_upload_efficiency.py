import requests
import hashlib
import os
import time

CLIENT_ID = "XUDOMtBDQE_ftzZwDKS7PM3naIl6r__Sz69LrhbyBk8"
CLIENT_SECRET = "41pRyqRmWUZhwnWssLvAw67eVA7NVDPQKkzyiSSfwjw"
BASE_URL = "https://sleephq.com"

def get_token():
    url = f"{BASE_URL}/oauth/token"
    
    # Try 1: Exact match to C++ string construction
    # C++: body += "&scope=read+write"; -> This sends "read+write" which decodes to "read write"
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    data = f"grant_type=password&client_id={CLIENT_ID}&client_secret={CLIENT_SECRET}&scope=read+write"
    
    print(f"Requesting token with body: {data}")
    response = requests.post(url, headers=headers, data=data)
    
    if response.status_code == 200:
        return response.json().get("access_token")
    
    print(f"Token attempt 1 failed: {response.text}")
    
    # Try 2: scope=api (common alternative)
    data = f"grant_type=password&client_id={CLIENT_ID}&client_secret={CLIENT_SECRET}&scope=api"
    response = requests.post(url, headers=headers, data=data)
    if response.status_code == 200:
        return response.json().get("access_token")
        
    # Try 3: No scope
    data = f"grant_type=password&client_id={CLIENT_ID}&client_secret={CLIENT_SECRET}"
    response = requests.post(url, headers=headers, data=data)
    if response.status_code == 200:
        return response.json().get("access_token")

    print(f"All token attempts failed.")
    return None

def get_team_id(token):
    url = f"{BASE_URL}/api/v1/me"
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.api+json"
    }
    response = requests.get(url, headers=headers)
    if response.status_code == 200:
        return response.json().get("data", {}).get("current_team_id")
    return None

import json

def create_import(token, team_id):
    url = f"{BASE_URL}/api/v1/teams/{team_id}/imports"
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.api+json",
        "Content-Type": "application/vnd.api+json"
    }
    payload = {
        "data": {
            "type": "imports",
            "attributes": {
                # "team_id": team_id  # team_id is in URL now
            }
        }
    }
    # Must use data=json.dumps() to preserve custom Content-Type
    print(f"Creating import for team {team_id}...")
    response = requests.post(url, headers=headers, data=json.dumps(payload))
    
    if response.status_code == 201:
        return response.json().get("data", {}).get("id")
    else:
        print(f"Create import error: {response.status_code} {response.text}")
        return None

def upload_file_hash_last(token, import_id, file_path, remote_path):
    url = f"{BASE_URL}/api/v1/imports/{import_id}/files"
    
    file_name = os.path.basename(file_path)
    with open(file_path, "rb") as f:
        file_content = f.read()
    
    content_hash = hashlib.md5(file_content + file_name.encode()).hexdigest()
    
    boundary = "----TestBoundary" + str(int(time.time()))
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.api+json",
        "Content-Type": f"multipart/form-data; boundary={boundary}"
    }
    
    body = b""
    
    # Name part
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="name"\r\n\r\n'
    body += file_name.encode()
    body += b"\r\n"
    
    # Path part
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="path"\r\n\r\n'
    body += remote_path.encode()
    body += b"\r\n"
    
    # File part
    body += f"--{boundary}\r\n".encode()
    body += f'Content-Disposition: form-data; name="file"; filename="{file_name}"\r\n'.encode()
    body += b"Content-Type: application/octet-stream\r\n\r\n"
    body += file_content
    body += b"\r\n"
    
    # Hash part (LAST)
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="content_hash"\r\n\r\n'
    body += content_hash.encode()
    body += b"\r\n"
    
    body += f"--{boundary}--\r\n".encode()
    
    print(f"\nUploading {file_name} with hash LAST...")
    response = requests.post(url, headers=headers, data=body)
    
    print(f"Status: {response.status_code}")
    # print(f"Body: {response.text}")
    
    return response.status_code == 201 or response.status_code == 200

def upload_batch(token, import_id, file_paths, remote_path):
    url = f"{BASE_URL}/api/v1/imports/{import_id}/files"
    
    boundary = "----BatchBoundary" + str(int(time.time()))
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.api+json",
        "Content-Type": f"multipart/form-data; boundary={boundary}"
    }
    
    body = b""
    
    print(f"\nAttempting batch upload of {len(file_paths)} files (unlikely to work but testing)...")
    
    for file_path in file_paths:
        file_name = os.path.basename(file_path)
        with open(file_path, "rb") as f:
            file_content = f.read()
        content_hash = hashlib.md5(file_content + file_name.encode()).hexdigest()
        
        # Name
        body += f"--{boundary}\r\n".encode()
        body += b'Content-Disposition: form-data; name="name[]"\r\n\r\n'
        body += file_name.encode()
        body += b"\r\n"
        
        # Hash
        body += f"--{boundary}\r\n".encode()
        body += b'Content-Disposition: form-data; name="content_hash[]"\r\n\r\n'
        body += content_hash.encode()
        body += b"\r\n"
        
        # File
        body += f"--{boundary}\r\n".encode()
        body += f'Content-Disposition: form-data; name="file[]"; filename="{file_name}"\r\n'.encode()
        body += b"Content-Type: application/octet-stream\r\n\r\n"
        body += file_content
        body += b"\r\n"

    # Path (common)
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="path"\r\n\r\n'
    body += remote_path.encode()
    body += b"\r\n"
        
    body += f"--{boundary}--\r\n".encode()
    
    response = requests.post(url, headers=headers, data=body)
    print(f"Batch Status: {response.status_code}")
    print(f"Batch Body: {response.text}")

def main():
    token = get_token()
    if not token:
        return
        
    team_id = get_team_id(token)
    print(f"Team ID: {team_id}")
    
    import_id = create_import(token, team_id)
    print(f"Import ID: {import_id}")
    
    # Test 1: Hash Last
    test_file = "/root/sdcard2/Identification.json"
    if os.path.exists(test_file):
        upload_file_hash_last(token, import_id, test_file, "/")
    
    # Test 2: Batching
    files = ["/root/sdcard2/Identification.json", "/root/sdcard2/Identification.crc"]
    exists = all(os.path.exists(f) for f in files)
    if exists:
        upload_batch(token, import_id, files, "/")

if __name__ == "__main__":
    main()
