#!/usr/bin/python
import requests
import json
import os
import zipfile
import sys


def get_artifact_info():
    return requests.request("get", "https://api.github.com/repos/RA3BattleNet/Relay/actions/artifacts").text


def get_latest_artifact(data):
    if data["artifacts"]:
        for artifact in data["artifacts"]:
            if artifact["name"] == "artifact":
                artifact_url = artifact["archive_download_url"]
                artifact_id = artifact["id"]
                return artifact_id, artifact_url
    else:
        return None, None


def download_artifact(url, token, file):
    headers = {"Accept": "application/vnd.github+json", "Authorization": "token %s" % token}
    response = requests.request("get", url, headers = headers)
    open(file, 'wb+').write(response.content)


def extract(src, dst):
    with zipfile.ZipFile(src, 'r') as zip:
        zip.extractall(dst)


def download_then_extract(token, file, folder):
    data = json.loads(get_artifact_info())
    id, url = get_latest_artifact(data)
    if id and url:
        download_artifact(url, token, file)
        extract(file, folder)


if __name__ == "__main__":
    args = sys.argv
    print("Downloading RA3BattleNet/Relay...")
    print("INFO: Please make sure you've set up the required environment!")
    if len(args) <= 1:
        print("ERROR: Please specify a Github Access Token to download artifacts! Aborting...")
    else:
        print("INFO: Set token to: %s" % str(args[1]))
        download_then_extract(str(args[1]), "./relay.zip", "./")
        os.chmod("./Relay", 0o744)
        os.remove("./relay.zip")
        print("Download completed!")
