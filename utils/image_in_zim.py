import argparse
from pathlib import Path
from libzim.reader import Archive

def analyze_zim_images(zim_file_path):
    path = Path(zim_file_path)
    
    if not path.exists():
        print(f"[-] Error: The file '{path.absolute()}' does not exist.")
        return

    print(f"[+] Successfully opened: {path.name}")
    
    try:
        archive = Archive(path)
    except Exception as e:
        print(f"[-] Error: Failed to read the ZIM archive. Details: {e}")
        return

    print("\n================ ARCHIVE INFORMATION ================")
    
    # Using the verified built-in property for the total count
    if hasattr(archive, "all_entry_count"):
        print(f"[i] Total Entries: {archive.all_entry_count:,}")
        
    # Checking the metadata header for media
    if hasattr(archive, "media_count"):
        print(f"[i] Media Count: {archive.media_count:,} media files reported.")
        if archive.media_count > 0:
            print("[✓] Success! The metadata confirms this ZIM file contains media/images.")
        else:
            print("[X] Notice: Media count is 0. This implies no images are inside.")
            
    print("\n[+] Hunting for a few image path examples...")
    try:
        image_samples = []
        image_extensions = ('.jpg', '.jpeg', '.png', '.svg', '.webp', '.gif')
        
        # Cap the scan to the first 100,000 entries so massive files don't freeze your terminal
        total_entries = getattr(archive, "all_entry_count", 100000)
        scan_limit = min(100000, total_entries) 
        
        for index in range(scan_limit):
            entry = archive._get_entry_by_id(index)
            
            # The namespace is now baked into the path (e.g., 'I/image.png' or 'M/logo.jpg')
            # We check if it explicitly starts with 'I/' (Image directory) or ends with an image extension
            p = entry.path.lower()
            if p.startswith('i/') or p.endswith(image_extensions):
                image_samples.append(entry.path)
                
                # Stop searching once we find 5 examples
                if len(image_samples) >= 5:
                    break
                    
        if image_samples:
            print(f"Found examples in the first {scan_limit:,} entries:")
            for sample in image_samples:
                print(f"  - {sample}")
        else:
            print(f"No image paths found in the initial {scan_limit:,} scanned entries.")
            
    except Exception as e:
        print(f"[-] Sample extraction failed: {e}")

    print("=====================================================\n")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Safely inspect a Wikipedia .zim file for media and image layouts."
    )
    parser.add_argument(
        "file_path", 
        type=str, 
        help="The path to the .zim file you want to inspect"
    )
    
    args = parser.parse_args()
    analyze_zim_images(args.file_path)