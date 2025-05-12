#!/usr/bin/env python2

import os
import os.path
import platform
import subprocess
import re
import shutil
import sys
import tarfile
import xml.etree.ElementTree as ET
import json
import tempfile
import argparse
import glob
import zipfile
import shutil

# These are the Anki modules/packages:
import binary_conversion
import validate_anim_data

THIS_DIR = os.path.dirname(os.path.realpath(__file__))

sys.path.insert(0, os.path.join(THIS_DIR, '..', '..', 'tools', 'build', 'tools'))

import ankibuild.util
import ankibuild.deptool

# configure unbuffered output
# https://stackoverflow.com/a/107717/217431
class Unbuffered(object):
   def __init__(self, stream):
       self.stream = stream
   def write(self, data):
       self.stream.write(data)
       self.stream.flush()
   def writelines(self, datas):
       self.stream.writelines(datas)
       self.stream.flush()
   def __getattr__(self, attr):
       return getattr(self.stream, attr)

# This is a work-around for not passing -u via /usr/bin/env on linux
sys.stdout = Unbuffered(sys.stdout)

VERBOSE = True
# Replace True for name of log file if desired.
REPORT_ERRORS = True
RETRIES = 10
SVN_INFO_CMD = "svn info %s %s --xml"
SVN_LS_CMD = "svn ls -R %s"
SVN_CRED = "--username %s --password %s --no-auth-cache --non-interactive --trust-server-cert"
PROJECT_ROOT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
DEPS_FILE = os.path.join(PROJECT_ROOT_DIR, 'DEPS')
EXTERNALS_DIR = os.path.join(PROJECT_ROOT_DIR, 'EXTERNALS')
THIRD_PARTY_DIR = os.path.join(PROJECT_ROOT_DIR, '3rd')
DIFF_BRANCH_MSG = "is already a working copy for a different URL"

# Most animation tar files in SVN are packages of JSON files that should be unpacked in the root
# "animations" directory, but facial animation tar files (packages of PNG files) should be unpacked
# in a subdirectory of the root "spriteSequences" directory. The following list indicates the groups
# of tar files that should be unpacked in a subdirectory, which is named after the tar file.
# TODO: Put this info (unpack files in root directory or subdirectory) somewhere in the DEPS file.
UNPACK_INTO_SUBDIR = ["spriteSequences", "independentSprites"]

MANIFEST_FILE_NAME = "anim_manifest.json"
MANIFEST_NAME_KEY = "name"
MANIFEST_LENGTH_KEY = "length_ms"

# Trigger asset validation when any of the following external dependencies are updated.
# Since one of the validation steps is to confirm that all audio used in animations is in
# fact available, we trigger that validation if either the animation or the audio is updated.
ASSET_VALIDATION_TRIGGERS = ["victor-animation-assets", "victor-audio-assets"]

def get_anki_deps_cache_directory():
   anki_deps_cache_dir = os.path.join(os.path.expanduser("~"), ".anki", "deps-cache")
   ankibuild.util.File.mkdir_p(anki_deps_cache_dir)
   return anki_deps_cache_dir


def get_anki_sha256_cache_directory():
   anki_sha256_cache_dir = os.path.join(get_anki_deps_cache_directory(), "sha256")
   ankibuild.util.File.mkdir_p(anki_sha256_cache_dir)
   return anki_sha256_cache_dir


def get_anki_svn_cache_directory(url = None):
   anki_svn_cache_dir = os.path.join(get_anki_deps_cache_directory(), "svn")
   if url:
      url = url.replace('/', '-')
      anki_svn_cache_dir = os.path.join(anki_svn_cache_dir, url)
   ankibuild.util.File.mkdir_p(anki_svn_cache_dir)
   return anki_svn_cache_dir

def get_anki_file_cache_directory(url = None):
   anki_file_cache_dir = os.path.join(get_anki_deps_cache_directory(), "files")
   if url:
      url = url.replace('/', '-')
      anki_file_cache_dir = os.path.join(anki_file_cache_dir, url)
   ankibuild.util.File.mkdir_p(anki_file_cache_dir)
   return anki_file_cache_dir

def get_anki_svn_cache_tarball(url, rev):
   anki_svn_cache_dir = get_anki_svn_cache_directory(url)
   tarball = os.path.join(anki_svn_cache_dir, "r" + str(rev) + ".tgz")
   return tarball

def extract_svn_cache_tarball_to_directory(url, rev, loc):
   tarball = get_anki_svn_cache_tarball(url, rev)
   if not os.path.isfile(tarball):
      return False
   ankibuild.util.File.rm_rf(loc)
   ankibuild.util.File.mkdir_p(loc)
   ptool = "tar"
   ptool_options = ['-v', '-x', '-z', '-f']
   unpack = [ptool] + ptool_options + [tarball, '-C', loc]
   pipe = subprocess.Popen(unpack, stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=True)
   (stdout, stderr) = pipe.communicate()
   status = pipe.poll()
   if status != 0:
      ankibuild.util.File.rm_rf(loc)
   return status == 0

def save_svn_cache_tarball_from_directory(url, rev, loc, cred):
   tarball = get_anki_svn_cache_tarball(url, rev)
   tmp_tarball = tarball + ".tmp"
   ankibuild.util.File.rm_rf(tarball)
   ankibuild.util.File.rm_rf(tmp_tarball)
   svn_ls_cmd = SVN_LS_CMD % (cred)
   pipe = subprocess.Popen(svn_ls_cmd.split(), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, close_fds=True, cwd=loc)
   (stdoutdata, stderrdata) = pipe.communicate()
   status = pipe.poll()
   if status != 0:
      return False
   files = stdoutdata.split('\n')
   with tarfile.open(tmp_tarball, "w:gz") as tar:
      tar.add(os.path.join(loc, '.svn'), arcname = '.svn', recursive = True)
      for f in files:
         if f:
            fullpath = os.path.join(loc, f)
            if os.path.exists(fullpath):
               tar.add(fullpath, arcname = f, recursive = False)
   os.rename(tmp_tarball, tarball)


def is_tool(name):
    """
    Test if application exists. Equivalent to which.
    stolen from http://stackoverflow.com/questions/11210104/check-if-a-program-exists-from-a-python-script

    Args:
        name: string

    Returns: bool

    """
    try:
        devnull = open(os.devnull)
        subprocess.Popen([name], stdout=devnull, stderr=devnull, close_fds=True).communicate()
    except OSError as e:
        if e.errno == os.errno.ENOENT:
            return False
    return True


def is_up(url_string):
    import urllib2
    try:
        response = urllib2.urlopen(url_string, None, 10)
        response.read()
        return True
    except urllib2.HTTPError, e:
        if e.code == 401:
            # Authentication error site is up and requires login.
            return True
        print(e.code)
        return False
    except urllib2.URLError, e:
        return False
    except ConnectionResetError, e:
        response.close()
        os.exit(e)


def sha256sum(filename):
    if not os.path.isfile(filename):
       return None
    import hashlib
    sha256 = hashlib.sha256()
    with open(filename, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            sha256.update(chunk)
    return sha256.hexdigest()


def read_int_from_file(filename):
   val = None
   if filename and os.path.isfile(filename):
      with open(filename, 'r') as f:
         data = f.read().strip()
         val = int(data)
   return val

def write_int_to_file(filename, val):
   with open(filename, 'w') as f:
      f.write(str(val))


def is_valid_checksum(url_string):
    """
    Test if the provided checksum is a valid SHA and uncorrupted. Function will pull checksums
    from json DEPS then cross compare its sha value, and return True or False
    if the checksum is correct.
    # Logic from https://codereview.stackexchange.com/a/27831

    Args:
        url_string: root_url from DEPS

    Returns: hex digest of checksum(sha256 in this case)

    """
    import urllib2
    import contextlib
    import hashlib

    # TODO: add other checksum verifications as new checksums values are added for dependencies

    hash_url = hashlib.sha256()  # Creates a SHA-256 hash object, stored into hash_url.
    # Return a context manager that closes _thing_ upon completion of the block.
    with contextlib.closing(urllib2.urlopen(url_string)) as binary_file:
        # Open requested url and then store it as a binary file
        for chunk in iter(lambda: binary_file.read(4096), ''):
            # iterate through the binary_file's chunks at 4096b intervals
            hash_url.update(chunk)
            # Update the hash object with the bytes-like object.
        return hash_url.hexdigest()  # Return the hexadecimal of the data passed to the update() method so far.


def extract_files_from_tar(extract_dir, file_types, put_in_subdir=False):
  """
  Given the path to a directory that contains .tar files and a list
  of file types, eg. [".json", ".png"], this function will unpack
  the given file types from all the .tar files.  If the optional
  'put_in_subdir' input argument is set to True, then the files are
  unpacked into a sub-directory named after the .tar file.
  """
  anim_name_length_mapping = {}

  for (dir_path, dir_names, file_names) in os.walk(extract_dir):
    # Generate list of all .tar files in/under the directory provided by the caller (extract_dir)
    all_files = map(lambda x: os.path.join(dir_path, x), file_names)
    tar_files = [a_file for a_file in all_files if a_file.endswith('.tar')]

    if tar_files and not put_in_subdir:
      # If we have any .tar files to unpack and they will NOT be unpacked into a sub-directory,
      # then first clean up existing files that may conflict with what will be unpacked. For
      # example, if a .tar file previously contained foo.json and it now contains bar.json, we
      # don't want foo.json lingering from a previous unpacking.
      # PS, if the .tar files WILL be unpacked into a sub-directory, we don't need to do any cleanup
      # here because unpack_tarball() will first delete the sub-directory if it already exists.
      file_types_to_cleanup = file_types + [binary_conversion.BIN_FILE_EXT]
      delete_files_from_dir(file_types_to_cleanup, dir_path, file_names)

    for tar_file in tar_files:
      anim_name_length_mapping.update(unpack_tarball(tar_file, file_types, put_in_subdir))
      # No need to remove the tar file here after unpacking it - all tar files are
      # deleted from cozmo_resources/assets/ on the device by Builder.cs

    if put_in_subdir:
      # If we are extracting tar files into subdirs, don't recurse into those subdirs.
      break

  return anim_name_length_mapping


def _get_specific_members(members, file_types):
  file_list = []
  for tar_info in members:
    if os.path.splitext(tar_info.name)[1] in file_types:
      file_list.append(tar_info)
  return file_list


def insert_source_rev_data_into_json(tar_file, tar_file_rev, json_file):
  #print("Inserting source file revision info into: %s" % json_file)
  metadata = {'tar_file' : tar_file,
              'tar_version' : tar_file_rev}
  metadata = {'metadata' : metadata}
  with open(json_file, 'a') as file_obj:
    file_obj.write("%s%s" % (json.dumps(metadata), os.linesep))


def delete_files_from_dir(file_types, dir_path, file_names):
  delete_count = 0
  file_types = [str(x) for x in file_types]
  #print("Deleting all %s files from %s" % (file_types, dir_path))
  for file_name in file_names:
    file_ext = str(os.path.splitext(file_name)[1])
    if file_ext in file_types:
      os.remove(os.path.join(dir_path, file_name))
      delete_count += 1
  #print("Deleted %s files of types %s" % (delete_count, file_types))
  return delete_count


def get_file_stats(which_dir):
  file_stats = {}
  for (dir_path, dir_names, file_names) in os.walk(which_dir):
    for file_name in file_names:
      file_ext = str(os.path.splitext(file_name)[1])
      if file_ext not in file_stats:
        file_stats[file_ext] = 1
      else:
        file_stats[file_ext] += 1
  return file_stats

def get_flatc_dir():
  """Determine flatc executable location for platform"""
  platform_map = {
    'Darwin': 'x86_64-apple-darwin',
    'Darwin-arm64': 'aarch64-apple-darwin',
    'Linux': 'x86_64-linux-gnu',
    'Linux-arm64': 'aarch64-linux-gnu'
  }
  platform_name = platform.system()
  if platform.machine() in ['aarch64', 'arm64']:
    platform_name = '{}-arm64'.format(platform_name)
  target_triple = platform_map.get(platform_name)

  if target_triple:
    flatc_dir = os.path.join(THIRD_PARTY_DIR,
                             'flatbuffers', 'host-prebuilts',
                             'current', target_triple, 'bin')
  else: 
    # default
    flatc_dir = os.path.join(THIRD_PARTY_DIR,
                             'flatbuffers', 'mac', 'Release')
  #print(flatc_dir)
  return flatc_dir
  

def convert_json_to_binary(json_files, bin_name, dest_dir, flatc_dir):
    tmp_json_files = []
    tmp_dir = tempfile.mkdtemp()
    for json_file in json_files:
        json_dest = os.path.join(tmp_dir, os.path.basename(json_file))
        shutil.move(json_file, json_dest)
        tmp_json_files.append(json_dest)
    bin_name = bin_name.lower()
    try:
        bin_file = binary_conversion.main(tmp_json_files, bin_name, flatc_dir)
    except StandardError, e:
        print("%s: %s" % (type(e).__name__, e.message))
        # If binary conversion failed, use the json files...
        for json_file in tmp_json_files:
            json_dest = os.path.join(dest_dir, os.path.basename(json_file))
            shutil.move(json_file, json_dest)
            print("Restored %s" % json_dest)
    else:
        bin_dest = os.path.join(dest_dir, bin_name)
        shutil.move(bin_file, bin_dest)
        shutil.rmtree(tmp_dir)


def unpack_tarball(tar_file, file_types=[], put_in_subdir=False, add_metadata=False, convert_to_binary=True):

  # TODO: Set 'add_metadata' to True when we are ready to start inserting
  # metadata into the JSON files to indicate which .tar file and version
  # the data came from.

  anim_name_length_mapping = {}

  # If this function should convert .json data to binary, that conversion will be done with "flatc",
  # so we need to specify the path to the directory that contains that FlatBuffers tool. See
  # https://google.github.io/flatbuffers/flatbuffers_guide_using_schema_compiler.html for additional
  # info about the "flatc" schema compiler.
  flatc_dir = get_flatc_dir()

  # Set the destination directory where the contents of the .tar file will be unpacked.
  dest_dir = os.path.dirname(tar_file)
  if put_in_subdir:
    subdir = os.path.splitext(os.path.basename(tar_file))[0]
    dest_dir = os.path.join(dest_dir, subdir)
    if os.path.isdir(dest_dir):
      # If the destination sub-directory already exists, get rid of it.
      shutil.rmtree(dest_dir)

#  tar_file_rev = get_svn_file_rev(tar_file)

  try:
    tar = tarfile.open(tar_file)
  except tarfile.ReadError, e:
    raise RuntimeError("%s: %s" % (e, tar_file))

  if file_types:
    tar_members = _get_specific_members(tar, file_types)
    #print("Unpacking %s (version %s) (%s files)" % (tar_file, tar_file_rev, len(tar_members)))
    tar.extractall(dest_dir, members=tar_members)
    tar.close()
    sprite_sequence = os.path.basename(os.path.dirname(tar_file)) in UNPACK_INTO_SUBDIR
    if ".json" in file_types and not sprite_sequence:
      json_files = [tar_info.name for tar_info in tar_members if tar_info.name.endswith(".json")]
      json_files = map(lambda x: os.path.join(dest_dir, x), json_files)
      if json_files:
        for json_file in json_files:
          anim_name_length_mapping.update(validate_anim_data.get_anim_name_and_length(json_file))
        if add_metadata:
          for json_file in json_files:
            insert_source_rev_data_into_json(tar_file, tar_file_rev, json_file)
        if convert_to_binary:
          bin_name = os.path.splitext(os.path.basename(tar_file))[0] + binary_conversion.BIN_FILE_EXT
          convert_json_to_binary(json_files, bin_name, dest_dir, flatc_dir)
  else:
    #print("Unpacking %s (version %s) (all files)" % (tar_file, tar_file_rev))
    tar.extractall(dest_dir)
    tar.close()

  return anim_name_length_mapping


def get_svn_file_rev(file_from_svn, cred=''):
  """
  Given a file that was checked out from SVN, this function will
  return the revision of that file in the form of an integer. If
  the revision cannot be determined, this function returns None.
  """
  svn_info_cmd = SVN_INFO_CMD % (cred, file_from_svn)
  #print("Running: %s" % svn_info_cmd)
  p = subprocess.Popen(svn_info_cmd.split(), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, close_fds=True)
  (stdout, stderr) = p.communicate()
  status = p.poll()
  if status != 0:
    return None
  root = ET.fromstring(stdout.strip())
  for entry in root.iter('entry'):
    try:
      rev = entry.attrib['revision']
    except KeyError:
      pass
    else:
      rev = int(rev)
      return rev


def svn_checkout(url, r_rev, loc, cred, checkout, cleanup, unpack, package, allow_extra_files,
                 stale_warning, abort_if_offline=True, verbose=VERBOSE):
    status = 0
    err = ''
    successful = ''
    # Try to import an svn tarball from the cache before contacting the server
    need_to_cache_svn_checkout = not extract_svn_cache_tarball_to_directory(url, r_rev, loc)
    if need_to_cache_svn_checkout:
       if not is_up(url):
          msg = 'Could not contact svn server at {0}. This URL may require VPN or local LAN access.'.format(url)
          if abort_if_offline:
              raise RuntimeError(msg)
          else:
              print(msg)
              print(stale_warning)
              return ''

       pipe = subprocess.Popen(checkout, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, close_fds=True)
       successful, err = pipe.communicate()
       status = pipe.poll()
       #print("status = %s" % status)

    if err == '' and status == 0:
        print(successful.strip())
        # Equivalent to a git clean
        pipe = subprocess.Popen(cleanup, stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=True)
        extract_message, error = pipe.communicate()
        if extract_message != '' and not allow_extra_files:
            last_column = re.compile(r'\S+\s+(\S+)')
            unversioned_files = last_column.findall(extract_message)
            for a_file in unversioned_files:
                if os.path.isdir(a_file):
                    shutil.rmtree(a_file)
                elif os.path.isfile(a_file):
                    os.remove(a_file)
        if need_to_cache_svn_checkout:
           save_svn_cache_tarball_from_directory(url, r_rev, loc, cred)
        if os.path.isfile(package):
            # call waits for the result.  Moving on to the next checkout doesnt need this to finish.
            pipe = subprocess.Popen(unpack, stdout=subprocess.PIPE, stderr=subprocess.PIPE, close_fds=True)
            successful, err = pipe.communicate()
            if verbose:
                print(err)
        return ''
    else:
        return err


import urllib2  # Python 2, for Python 3 use 'import urllib.request as urllib2'

def svn_package(svn_dict):
    """
    Args:
        svn_dict: dict

    Returns: List of names for the repos that were checked out
    """
    checked_out_repos = []

    # Repos and settings
    repos = svn_dict.get("repo_names", "")
    root_url = svn_dict.get("root_url", "")  # The base URL for your local HTTP server
    stale_warning = "WARNING: If this build succeeds, it may contain stale external data"

    for repo in repos:
        version = repos[repo].get("version", None)
        branch = repos[repo].get("branch", "trunk")
        export_dirname = repos[repo].get("export_dirname", repo)
        loc = os.path.join(DEPENDENCY_LOCATION, export_dirname)
        sub_dirs = repos[repo].get("subdirs", [])
        additional_files = repos[repo].get("additional_files", [])
        extract_types = repos[repo].get("extract_types_from_tar", [])

	# WIRE: UNCOMMENT THIS TO NOT BUILD ANIMATIONS
        #Move on if the directory already exists
        #if os.path.isdir(loc):
        #    print(export_dirname + " already exists!")
        #    continue

        # Download from the local HTTP server
        #print("Downloading " + export_dirname + "...")
        #if version is not None:
        #    remote_loc = os.path.join(root_url, svn_dict.get("main_folder", ""), repo + '-' + version + ".zip")
        #else:
        #    remote_loc = os.path.join(root_url, svn_dict.get("main_folder", ""), repo + ".zip")
        #local_loc = loc + ".zip"

        #try:
        #    # Fetch file from the HTTP server
        #    print("Fetching file from HTTP server: " + remote_loc)
        #    response = urllib2.urlopen(remote_loc)
        #    with open(local_loc, 'wb') as local_file:
        #        local_file.write(response.read())

        #except urllib2.HTTPError as e:
        #    if e.code == 404:
        #        print("The object does not exist on the server.")
        #    else:
        #        raise

        # Extract the assets
        #print("Extracting " + export_dirname + "...")
        #with zipfile.ZipFile(local_loc, 'r') as zip_ref:
        #    zip_ref.extractall(DEPENDENCY_LOCATION)
        #os.remove(local_loc)  # Remove the ZIP after extraction

        # Organize the assets into the required directory structure
        #os.rename(os.path.join(DEPENDENCY_LOCATION, repo), loc)

        #for sub_dir in os.listdir(loc):
        #    if sub_dir != branch:
        #        shutil.rmtree(os.path.join(loc, sub_dir))

        #if len(sub_dirs):
        #    for sub_dir in sub_dirs:
        #        src_loc = os.path.join(loc, branch, sub_dir)
        #        dst_loc = os.path.join(loc, sub_dir)
        #        shutil.move(src_loc, dst_loc)
        #else:
        #    for sub_dir in os.listdir(os.path.join(loc, branch)):
        #        src_loc = os.path.join(loc, branch, sub_dir)
        #        dst_loc = os.path.join(loc, sub_dir)
        #        shutil.move(src_loc, dst_loc)
        #shutil.rmtree(os.path.join(loc, branch))

        #Extract tar files if necessary
        if extract_types:
            print("Extracting tar files from SVN assets...")
            for sub_dir in sub_dirs:
                #print(sub_dir)
                put_in_sub_dir = os.path.basename(sub_dir) in UNPACK_INTO_SUBDIR
                try:
                    anim_name_length_mapping = extract_files_from_tar(os.path.join(loc, sub_dir), extract_types, put_in_sub_dir)
                except EnvironmentError as e:
                    anim_name_length_mapping = {}
                    print("Failed to unpack one or more tar files in [%s] because: %s" % (sub_dir, e))
                    print(stale_warning)
                file_stats = get_file_stats(sub_dir)
                if anim_name_length_mapping:
                    write_animation_manifest(loc, anim_name_length_mapping, additional_files)
                print("After unpacking tar files, '%s' contains the following files: %s"
                      % (os.path.basename(sub_dir), file_stats))

    return checked_out_repos


    # Old code below: Retrieves and extracts SVN assets using Anki servers
    """
       for subdir in subdirs:
         if not os.path.isdir(subdir):
           l_rev = 0
           break
       if l_rev != 0 and os.path.isdir(loc):
           l_rev = get_svn_file_rev(loc, cred)
           #print("The version of [%s] is [%s]" % (loc, l_rev))
           if l_rev is None:
               l_rev = 0
               msg = "Clearing out [%s] directory before getting a fresh copy."
               if subdirs:
                   for subdir in subdirs:
                       if os.path.exists(subdir):
                           print(msg % subdir)
                           shutil.rmtree(subdir)
               elif os.path.exists(loc):
                   print(msg % loc)
                   shutil.rmtree(loc)
       else:
           l_rev = 0

       try:
           r_rev = int(r_rev)
       except ValueError:
           pass

       no_update_msg  = "{0} does not need to be updated.  ".format(repo)
       no_update_msg += "Current {0} revision at {1}".format(tool, l_rev)

       # Do dependencies need to be checked out?
       need_to_checkout = (r_rev == "head" or l_rev != r_rev)

       # Check if manifest file exists
       manifest_file_path = os.path.join(loc, MANIFEST_FILE_NAME)
       manifest_file_exists = os.path.exists(manifest_file_path)

       if need_to_checkout or not manifest_file_exists:
           if need_to_checkout:
               print("Checking out '{0}'".format(repo))
               checked_out_repos.append(repo)
               err = svn_checkout(url, r_rev, loc, cred, checkout, cleanup,
                                  unpack, package, allow_extra_files, stale_warning)
               if err:
                   print("Error in checking out {0}: {1}".format(repo, err.strip()))
                   if DIFF_BRANCH_MSG in err:
                       print("Clearing out [%s] directory to replace it with a fresh copy" % loc)
                       shutil.rmtree(loc)
                       print("Checking out '{0}' again".format(repo))
                       err = svn_checkout(url, r_rev, loc, cred, checkout, cleanup,
                                          unpack, package, allow_extra_files, stale_warning)
                       if err:
                           print("Error in checking out {0}: {1}".format(repo, err.strip()))
                           print(stale_warning)
                   else:
                       print(stale_warning)
           else:
               print(no_update_msg)
           if extract_types:
               for subdir in subdirs:
                   put_in_subdir = os.path.basename(subdir) in UNPACK_INTO_SUBDIR
                   try:
                       anim_name_length_mapping = extract_files_from_tar(subdir, extract_types, put_in_subdir)
                   except EnvironmentError, e:
                       anim_name_length_mapping = {}
                       print("Failed to unpack one or more tar files in [%s] because: %s" % (subdir, e))
                       print(stale_warning)
                   file_stats = get_file_stats(subdir)
                   if anim_name_length_mapping:
                       write_animation_manifest(loc, anim_name_length_mapping, additional_files)
                   print("After unpacking tar files, '%s' contains the following files: %s"
                         % (os.path.basename(subdir), file_stats))

       else:
           print(no_update_msg)
    """

    return checked_out_repos


def write_animation_manifest(dest_dir, anim_name_length_mapping, additional_files=None, output_json_file=MANIFEST_FILE_NAME):
    if additional_files:
        additional_files = map(os.path.expandvars, additional_files)
        additional_files = map(os.path.abspath, additional_files)
        for additional_file in additional_files:
            if os.path.isfile(additional_file) and additional_file.endswith(".json"):
                print("Using this additional file for the animation manifest: %s" % additional_file)
                anim_name_length_mapping.update(validate_anim_data.get_anim_name_and_length(additional_file))
            elif os.path.isdir(additional_file):
                json_files = glob.glob(os.path.join(additional_file, '*.json'))
                for json_file in json_files:
                    print("Using this additional file for the animation manifest: %s" % json_file)
                    anim_name_length_mapping.update(validate_anim_data.get_anim_name_and_length(json_file))
            else:
                print("WARNING %s is an invalid path in 'additional_files'" % additional_file)

    all_anims = []
    for name, length in anim_name_length_mapping.iteritems():
        manifest_entry = {}
        manifest_entry[MANIFEST_NAME_KEY] = name
        manifest_entry[MANIFEST_LENGTH_KEY] = length
        all_anims.append(manifest_entry)

    output_data = json.dumps(all_anims, sort_keys=False, indent=2, separators=(',', ': '))
    output_file = os.path.join(dest_dir, output_json_file)
    if os.path.isfile(output_file):
        os.remove(output_file)
    elif not os.path.exists(dest_dir):
        os.makedirs(dest_dir)
    with open(output_file, 'w') as fh:
        fh.write(output_data)
    print("The animation manifest file (with %s entries) = %s" % (len(all_anims), output_file))


def git_package(git_dict):
    # WIP.  Should this function raise a NotImplementedError exception until it is completed?
    print("The git_package() function has NOT been implemented yet")
    return []


def files_package(files):
    pulled_files = []
    for file in files:
        url = files[file].get("url", None)
        sha256 = files[file].get("sha256", None)
        cached_file = os.path.join(get_anki_file_cache_directory(url), file)
        outfile = os.path.join(DEPENDENCY_LOCATION, file)
        if os.path.isfile(cached_file) and sha256 == sha256sum(cached_file):
           ankibuild.util.File.cp(cached_file, outfile)
           continue

        if os.path.isfile(outfile) and sha256 == sha256sum(outfile):
           ankibuild.util.File.cp(outfile, cached_file)
           continue

        import urllib2
        handle = urllib2.urlopen(url, None, 10)
        code = handle.getcode()
        if code < 200 or code >= 300:
           raise RuntimeError("Failed to download {0}. Check your network connection.  This URL may require VPN or local LAN access".format(url))

        tmpfile = outfile + ".tmp"
        download_file = open(tmpfile, 'w')
        block_size = 1024 * 1024
        for chunk in iter(lambda: handle.read(block_size), b''):
           download_file.write(chunk)

        download_file.close()

        download_hash = sha256sum(tmpfile)
        if sha256 != download_hash:
           ankibuild.util.File.rm_rf(tmpfile)
           raise RuntimeError("SHA256 Checksum mismatch.\nExpected: {0}\nActual:   {1}\nURL:      {2}".format(sha256, download_hash, url))

        os.rename(tmpfile, outfile)
        ankibuild.util.File.cp(outfile, cached_file)

    return pulled_files


def deptool_package(deptool_dict):
   deps_retrieved = []
   deps = deptool_dict.get("deps")
   project = deptool_dict.get("project")
   url_prefix = deptool_dict.get("url_prefix")
   for dep in deps:
      required_version = deps[dep].get("version", None)
      sha256_checksum = None
      checksums = deps[dep].get("checksums", None)
      if checksums:
         sha256_checksum = checksums.get("sha256", sha256_checksum)
      dst = os.path.join(DEPENDENCY_LOCATION, dep)
      if ankibuild.deptool.is_valid_dep_dir_for_version_and_sha256(dst, required_version, sha256_checksum):
         continue
      if os.path.islink(dst):
         os.unlink(dst)
      else:
         ankibuild.util.File.rm_rf(dst)
      src = ankibuild.deptool.find_or_install_dep(project, dep, required_version, url_prefix, sha256_checksum)
      if not src:
         raise RuntimeError('Could not find or install {0}/{1} (version = {2}, sha256 = {3}, url_prefix = {4})'
                            .format(project, dep, required_version, sha256_checksum, url_prefix))
      os.symlink(src, dst)
      deps_retrieved.append(dep)

   return deps_retrieved

def teamcity_package(tc_dict):
    cache_dir = get_anki_sha256_cache_directory()
    downloaded_builds = []
    teamcity=True
    tool = "curl"
    ptool = "tar"
    ptool_options = ['-v', '-x', '-z', '-f']
    assert is_tool(tool)
    assert is_tool(ptool)
    assert isinstance(tc_dict, dict)
    root_url = tc_dict.get("root_url", "undefined_url")
    password = tc_dict.get("pwd", "")
    user = tc_dict.get("default_usr", "undefined")
    builds = tc_dict.get("builds", "undefined")
    if not builds:
       return downloaded_builds

    if user == "undefined":
        # These artifacts are stored on artifactory.
        teamcity=False

    for build in builds:
        required_version = builds[build].get("version", None)
        unpack_path = os.path.join(DEPENDENCY_LOCATION, build)
        version_file_path = os.path.join(unpack_path, "VERSION")
        stored_version = read_int_from_file(version_file_path)
        if required_version == str(stored_version):
           continue

        # Clear out and re-create unpack destination
        ankibuild.util.File.rm_rf(unpack_path)
        ankibuild.util.File.mkdir_p(unpack_path)

        build_type_id = builds[build].get("build_type_id", "undefined")
        package_name = builds[build].get("package_name", "undefined")
        ext = builds[build].get("extension", "undefined")
        checksums = builds[build].get("checksums", None)
        sha = None
        if checksums:
           sha = checksums.get("sha256", "undefined")

        package = package_name + '.' + ext
        dist = os.path.join(DEPENDENCY_LOCATION, package)
        if sha:
           dist = os.path.join(cache_dir, sha)
           if os.path.isfile(dist) and sha != sha256sum(dist):
              os.remove(dist)
        else:
           ankibuild.util.File.rm_rf(dist)

        # Unlike svn_package, we need to unpack inside 'unpack_path' because the package
        # may not have its own folder structure
        # TODO:  Switch parameters and compression tool based on extension.
        unpack = [ptool] + ptool_options + [dist, '-C', unpack_path]

        # If we don't already have a cached copy of the package, download it
        if not os.path.isfile(dist):
           if not is_up(root_url):
              raise RuntimeError("Failed to reach {0}. Check your network connection.  This URL may require VPN or local LAN access.".format(root_url))

           if teamcity:
              combined_url = "{0}/repository/download/{1}/{2}/{3}_{4}.{5}".format(root_url,
                                                                                  build_type_id,
                                                                                  required_version,
                                                                                  package_name,
                                                                                  required_version,
                                                                                  ext)
              pull_down = [tool, '--user', "{0}:{1}".format(user, password), '-f', combined_url, '-o', dist]
           else:
              # Note the required_version is in the path and the name of the file.
              combined_url = "{0}/{1}/{2}/{4}/{3}_{4}.{5}".format(root_url,
                                                                  build,
                                                                  build_type_id,
                                                                  package_name,
                                                                  required_version,
                                                                  ext)
              pull_down = [tool, '-f', combined_url, '-o', dist]
           if VERBOSE:
              pull_down += ['-v']

           for n in range(RETRIES):
              pipe = subprocess.Popen(pull_down, stderr=subprocess.PIPE, stdout=subprocess.PIPE)
              curl_info, err = pipe.communicate()
              status = pipe.poll()
              if status == 0:
                 if sha:
                    downloaded_sha = sha256sum(dist)
                    if sha != downloaded_sha:
                       print("ERROR downloading {0}!!".format(package_name))
                       print("Expected hash of download: {0}".format(sha))
                       print("Actual hash of download: {0}".format(downloaded_sha))
                       os.remove(dist)
                       sys.exit("Assume binary corruption.")
                 print("{0} Downloaded.  New version {1} ".format(build.title(), required_version))
                 downloaded_builds.append(build.title())
                 break
              else:
                 print(err)
                 if os.path.isfile(dist):
                    os.remove(dist)
              print("ERROR {0}ing {1}.  {2} of {3} attempts.".format(tool, package, n+1, RETRIES))


        # Unpack package to destination
        pipe = subprocess.Popen(unpack, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        successful, err = pipe.communicate()
        if not sha:
           ankibuild.util.File.rm_rf(dist)
        if not os.path.isfile(version_file_path):
           write_int_to_file(version_file_path, required_version)
        if VERBOSE:
           print err

    return downloaded_builds


def extract_dependencies(version_file, location=EXTERNALS_DIR, validate_assets=True):
    """
    Entry point to starting the dependency extraction.

    :rtype: Null

    Args:
        version_file: path
        location: path
    """
    global DEPENDENCY_LOCATION
    DEPENDENCY_LOCATION = location
    ankibuild.util.File.mkdir_p(location)
    updated_deps = json_parser(version_file)
    updated_deps = map(str, updated_deps)
    if validate_assets and len(set(updated_deps) & set(ASSET_VALIDATION_TRIGGERS)) > 0:
        # At least one of the asset validation triggers was updated, so perform validation...
        validate_anim_data.check_anims_all_anim_groups(location)
        validate_anim_data.check_audio_events_all_anims(location)


def json_parser(version_file):
    """
    This function is used to parse the "DEPS" file and pull in
    any external dependencies.

    NOTE: This should pull in TeamCity dependencies, eg. CoreTech External,
    before SVN dependencies, eg. cozmo-assets, because the latter may
    depend on the former, eg. the Cozmo build uses FlatBuffers from
    CoreTech External to process animation data from cozmo-assets.

    Returns:
        object: Array

    Args:
        version_file: path
    """
    updated_deps = []
    if os.path.isfile(version_file):
        with open(version_file, mode="r") as file_obj:
            djson = json.load(file_obj)
            if "deptool" in djson:
                updated_deps.extend(deptool_package(djson["deptool"]))
            if "artifactory" in djson:
                updated_deps.extend(teamcity_package(djson["artifactory"]))
            if "teamcity" in djson:
                updated_deps.extend(teamcity_package(djson["teamcity"]))
            if "svn" in djson:
                updated_deps.extend(svn_package(djson["svn"]))
            if "git" in djson:
                updated_deps.extend(git_package(djson["git"]))
            if "files" in djson:
                updated_deps.extend(files_package(djson["files"]))
    else:
        sys.exit("ERROR: %s does not exist" % version_file)
    return updated_deps


def update_teamcity_version(version_file, teamcity_builds):
    """
    Update version entries of teamcity builds for dependencies.

    :rtype: Null

    Args:
        version_file: path

        teamcity_builds: dict
    """
    assert isinstance(teamcity_builds, dict)
    if os.path.isfile(version_file):
        djson = dict()
        with open(version_file, mode="r") as file_obj:
            djson = json.load(file_obj)
            assert isinstance(djson, dict)
            if "teamcity" in djson:
                tc_dict = djson["teamcity"]
                for build in tc_dict["builds"]:
                    if build in teamcity_builds:
                        djson["teamcity"]["builds"][build]["version"] = teamcity_builds[build]
            else:
                exit()
        with open(version_file, mode="w") as file_obj:
            file_obj.write(json.dumps(djson, sort_keys=True, indent=4, separators=(',', ': ')))


###############
# ENTRY POINT #
###############

def parse_args(argv=[]):
  parser = argparse.ArgumentParser(description='fetch external build deps')
  parser.add_argument('--verbose', dest='verbose', action='store_true',                             
                      help='prints extra output')
  parser.add_argument('--deps-file',
                      action='store',
                      default=DEPS_FILE,
                      help='path to DEPS file')
  parser.add_argument('--externals-dir',
                      action='store',
                      default=EXTERNALS_DIR,
                      help='path to EXTERNALS dir')                           
                                                                                                    
  (options, args) = parser.parse_known_args(argv)                                             
  return options


def main(argv):
    os.environ['PROJECT_ROOT_DIR'] = PROJECT_ROOT_DIR
    options = parse_args(argv[1:])
    deps_file = os.path.abspath(options.deps_file)
    externals_dir = os.path.abspath(options.externals_dir)
    if options.verbose:
        print("    deps-file: {}".format(deps_file))
        print("externals-dir: {}".format(externals_dir))
    extract_dependencies(deps_file, externals_dir)


if __name__ == '__main__':
    main(sys.argv)


